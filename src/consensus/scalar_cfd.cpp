// Copyright (c) 2026 The Tensorcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/scalar_cfd.h>

#include <arith_uint256.h>
#include <assets/asset.h> // assets::SCALAR_FORMAT_RAW_U256_LE

#include <algorithm>
#include <cstdint>

namespace {
//! Widen a 256-bit value into a 512-bit accumulator without truncation, using only the
//! public interface of base_uint (GetLow64 / shift / or). Mirrors the file-local helper in
//! consensus/difficulty_cfd.cpp (kept separate to avoid coupling the two modules' headers).
base_uint<512> Widen(const arith_uint256& x)
{
    base_uint<512> result;
    arith_uint256 tmp = x;
    for (int word = 0; word < 4; ++word) {
        base_uint<512> piece(tmp.GetLow64());
        piece <<= (64 * word);
        result |= piece;
        tmp >>= 64;
    }
    return result;
}
} // namespace

bool ScalarCfdFixingBuried(int publication_height, int context_height, int maturity_depth)
{
    if (publication_height < 0 || maturity_depth < 0) return false;
    // int64 arithmetic so adversarial/edge inputs cannot signed-overflow.
    const int64_t pub = publication_height;
    const int64_t ctx = context_height;
    const int64_t mat = maturity_depth;
    return pub < ctx && pub + mat <= ctx;
}

std::optional<ResolvedScalar> ResolveScalarFixing(
    const uint256& underlying_asset_id,
    uint32_t feed_id,
    uint64_t fixing_ref,
    uint32_t publication_deadline_height,
    const uint256& fallback_scalar,
    uint16_t leaf_scalar_format_id,
    int context_height,
    int maturity_depth,
    int fallback_grace,
    const std::function<std::optional<ScalarRecord>(const uint256&, uint32_t, uint64_t)>& reader)
{
    // Clamp the effective grace to >= maturity so the race-freedom invariant holds even if a
    // chainparam (mis)configures grace < maturity: an in-time fixing is then always buried
    // before the fallback branch below can fire, so the two are never simultaneously eligible.
    const int64_t mat = maturity_depth < 0 ? 0 : maturity_depth;
    const int64_t grace = fallback_grace < 0 ? 0 : fallback_grace;
    const int64_t effective_grace = std::max(grace, mat);

    // Branch 1: a USABLE real fixing wins (checked first). "Usable" = exists, in the leaf's
    // committed encoding, published at/before the deadline (late pubs ignored), and buried.
    if (const auto real = reader(underlying_asset_id, feed_id, fixing_ref);
        real
        && real->scalar_format_id == leaf_scalar_format_id
        && static_cast<int64_t>(real->publication_height) <= static_cast<int64_t>(publication_deadline_height)
        && ScalarCfdFixingBuried(real->publication_height, context_height, maturity_depth)) {
        return ResolvedScalar{real->scalar, real->scalar_format_id, /*is_fallback=*/false};
    }

    // Branch 2: past the deadline + effective grace with no usable real fixing -> committed
    // fallback (in the leaf's encoding). int64 arithmetic; deadline is a uint32 so deadline+grace
    // (both promoted to int64) cannot overflow.
    if (static_cast<int64_t>(context_height) >= static_cast<int64_t>(publication_deadline_height) + effective_grace) {
        return ResolvedScalar{fallback_scalar, leaf_scalar_format_id, /*is_fallback=*/true};
    }

    // Otherwise still pending: wait for burial or for the grace window to elapse.
    return std::nullopt;
}

bool ComputeScalarCfdPayout(const arith_uint256& strike,
                            const arith_uint256& realized,
                            ScalarLossDenominator denominator,
                            uint32_t lambda_q,
                            uint64_t vault_im,
                            bool short_leg,
                            uint64_t min_settle_output,
                            ScalarCfdPayout& out)
{
    if (lambda_q == 0) return false;
    if (vault_im < min_settle_output) return false;

    // Resolve the loss denominator up front (§4.2): STRIKE -> K, REALIZED -> X. Fail CLOSED on
    // an unknown mode — this is consensus math, so an out-of-range enum must never silently
    // settle as a default. (The leaf parser already rejects mode 2, but the guard is here too.)
    const arith_uint256* denom_ptr = nullptr;
    switch (denominator) {
    case ScalarLossDenominator::STRIKE:   denom_ptr = &strike;   break;
    case ScalarLossDenominator::REALIZED: denom_ptr = &realized; break;
    }
    if (denom_ptr == nullptr) return false;

    // Loss numerator by direction, in scalar (price) space. Flat / in-the-money: the owner
    // keeps the full margin (no division needed).
    arith_uint256 num;
    if (short_leg) {
        // short: loses as X rises above K.
        if (!(realized > strike)) {
            out.payout_owner = vault_im;
            out.payout_cp = 0;
            return true;
        }
        num = realized - strike;
    } else {
        // long: loses as X falls below K.
        if (!(realized < strike)) {
            out.payout_owner = vault_im;
            out.payout_cp = 0;
            return true;
        }
        num = strike - realized;
    }

    const arith_uint256& denom = *denom_ptr;

    // Clamped exact floor in 512 bits.
    //   raw fraction = lambda_q * num / (L * denom),  L = SCALARCFD_LAMBDA_SCALE = 2^16
    //   payout_cp    = floor(fraction * vault_im), clamped to vault_im once fraction >= 1.
    // Widths match difficulty: num,denom <= 2^256; lambda_q <= 2^32; vault_im <= 2^64 — so
    // lhs <= 2^288, denom_scaled <= 2^272, numer <= 2^352, all inside 512 bits.
    const base_uint<512> lhs = Widen(num) * base_uint<512>(static_cast<uint64_t>(lambda_q));
    const base_uint<512> denom_scaled = Widen(denom) << 16; // L * denom

    uint64_t payout_cp;
    if (lhs >= denom_scaled) {
        // Raw fraction >= 1.0 (or a zero denominator, num > 0) -> full liquidation. A zero
        // denom_scaled lands here (lhs > 0 >= 0), so the divide below is never reached with a
        // zero divisor.
        payout_cp = vault_im;
    } else {
        // lhs < denom_scaled implies denom_scaled > 0, so the division is well-defined and the
        // quotient < vault_im <= 2^64 fits in 64 bits.
        const base_uint<512> numer = lhs * base_uint<512>(vault_im);
        const base_uint<512> quotient = numer / denom_scaled;
        payout_cp = quotient.GetLow64();
    }
    uint64_t payout_owner = vault_im - payout_cp;

    // Dust floor: snap a sub-min non-zero leg to 0, its value going to the surviving leg.
    // vault_im >= min_settle_output guarantees at most one leg can be sub-min at a time.
    if (payout_cp != 0 && payout_cp < min_settle_output) {
        payout_cp = 0;
        payout_owner = vault_im;
    } else if (payout_owner != 0 && payout_owner < min_settle_output) {
        payout_owner = 0;
        payout_cp = vault_im;
    }

    out.payout_owner = payout_owner;
    out.payout_cp = payout_cp;
    return true;
}

bool DecodeScalarValue(uint16_t scalar_format_id, const uint256& raw, arith_uint256& out)
{
    switch (scalar_format_id) {
    case assets::SCALAR_FORMAT_RAW_U256_LE:
        out = UintToArith256(raw); // raw 256-bit LE: identity decode, every value canonical
        return true;
    default:
        return false; // unknown encoding -> opcode fails closed (SCALARCFD_ENCODING)
    }
}
