// Copyright (c) 2026 The Tensorcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/scalar_cfd.h>

#include <arith_uint256.h>
#include <consensus/difficulty_cfd.h> // MIN_SETTLE_OUTPUT + ComputeDiffCfdPayout (parity)
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(scalar_cfd_payout_tests, BasicTestingSetup)

namespace {
arith_uint256 A(uint64_t v) { return arith_uint256(v); }

constexpr uint32_t Q16(uint32_t lambda) { return lambda * static_cast<uint32_t>(SCALARCFD_LAMBDA_SCALE); }
constexpr uint64_t IM = 1'000'000;
const uint64_t MIN = static_cast<uint64_t>(MIN_SETTLE_OUTPUT);

// Convenience: compute and require success, returning the payout.
ScalarCfdPayout Payout(const arith_uint256& k, const arith_uint256& x, ScalarLossDenominator denom,
                       uint32_t lambda_q, uint64_t vault_im, bool short_leg, uint64_t min_out = 0)
{
    ScalarCfdPayout out;
    BOOST_REQUIRE(ComputeScalarCfdPayout(k, x, denom, lambda_q, vault_im, short_leg,
                                         min_out == 0 ? MIN : min_out, out));
    return out;
}

using D = ScalarLossDenominator;
} // namespace

// ---- Mode 0 (STRIKE-denominated) ------------------------------------------------------------

BOOST_AUTO_TEST_CASE(mode0_long_flat_or_itm_keeps_full_margin)
{
    // long loses only as X falls below K; X == K and X > K are flat/ITM.
    for (uint64_t x : {100u, 150u}) {
        const auto p = Payout(A(100), A(x), D::STRIKE, Q16(1), IM, /*short_leg=*/false);
        BOOST_CHECK_EQUAL(p.payout_owner, IM);
        BOOST_CHECK_EQUAL(p.payout_cp, 0u);
    }
}

BOOST_AUTO_TEST_CASE(mode0_long_partial_loss)
{
    // f_loss = 1 * (100-90)/100 = 0.10 -> cp = 100000.
    const auto p = Payout(A(100), A(90), D::STRIKE, Q16(1), IM, false);
    BOOST_CHECK_EQUAL(p.payout_cp, 100'000u);
    BOOST_CHECK_EQUAL(p.payout_owner, 900'000u);
}

BOOST_AUTO_TEST_CASE(mode0_long_half_loss)
{
    // f_loss = 1 * (100-50)/100 = 0.50 -> cp = 500000.
    const auto p = Payout(A(100), A(50), D::STRIKE, Q16(1), IM, false);
    BOOST_CHECK_EQUAL(p.payout_cp, 500'000u);
    BOOST_CHECK_EQUAL(p.payout_owner, 500'000u);
}

BOOST_AUTO_TEST_CASE(mode0_long_full_liquidation_on_clamp)
{
    // f_loss = 10 * (100-90)/100 = 1.0 (boundary) -> full liquidation.
    const auto p = Payout(A(100), A(90), D::STRIKE, Q16(10), IM, false);
    BOOST_CHECK_EQUAL(p.payout_cp, IM);
    BOOST_CHECK_EQUAL(p.payout_owner, 0u);
}

BOOST_AUTO_TEST_CASE(mode0_short_partial_and_flat)
{
    // short loses as X rises above K. X=110 -> cp = 1*(110-100)/100 = 0.10 -> 100000.
    const auto loss = Payout(A(100), A(110), D::STRIKE, Q16(1), IM, /*short_leg=*/true);
    BOOST_CHECK_EQUAL(loss.payout_cp, 100'000u);
    // X=90 (<K) -> no loss for a short -> owner full.
    const auto flat = Payout(A(100), A(90), D::STRIKE, Q16(1), IM, true);
    BOOST_CHECK_EQUAL(flat.payout_owner, IM);
    BOOST_CHECK_EQUAL(flat.payout_cp, 0u);
}

// ---- Mode 1 (REALIZED-denominated) ----------------------------------------------------------

BOOST_AUTO_TEST_CASE(mode1_long_partial_loss)
{
    // f_loss = 1 * (100-80)/80 = 0.25 -> cp = 250000.
    const auto p = Payout(A(100), A(80), D::REALIZED, Q16(1), IM, false);
    BOOST_CHECK_EQUAL(p.payout_cp, 250'000u);
    BOOST_CHECK_EQUAL(p.payout_owner, 750'000u);
}

BOOST_AUTO_TEST_CASE(mode1_long_full_when_ratio_reaches_one)
{
    // f_loss = 1 * (100-50)/50 = 1.0 -> full liquidation.
    const auto p = Payout(A(100), A(50), D::REALIZED, Q16(1), IM, false);
    BOOST_CHECK_EQUAL(p.payout_cp, IM);
}

BOOST_AUTO_TEST_CASE(mode1_short_partial_loss)
{
    // f_loss = 1 * (200-100)/200 = 0.50 -> cp = 500000.
    const auto p = Payout(A(100), A(200), D::REALIZED, Q16(1), IM, /*short_leg=*/true);
    BOOST_CHECK_EQUAL(p.payout_cp, 500'000u);
}

// ---- Zero-denominator -> full liquidation (no divide-by-zero) --------------------------------

BOOST_AUTO_TEST_CASE(zero_realized_denominator_long_is_full_liquidation)
{
    // REALIZED with X=0: |X-K|/X -> infinity -> full liquidation, NOT a divide-by-zero/failure.
    const auto p = Payout(A(100), A(0), D::REALIZED, Q16(1), IM, /*short_leg=*/false);
    BOOST_CHECK_EQUAL(p.payout_cp, IM);
    BOOST_CHECK_EQUAL(p.payout_owner, 0u);
}

BOOST_AUTO_TEST_CASE(zero_strike_denominator_short_is_full_liquidation)
{
    // STRIKE with K=0: a short loses the instant X>0; |X|/0 -> infinity -> full liquidation.
    const auto p = Payout(A(0), A(100), D::STRIKE, Q16(1), IM, /*short_leg=*/true);
    BOOST_CHECK_EQUAL(p.payout_cp, IM);
    // K=0 long: X<0 impossible -> always flat -> owner full (no division).
    const auto flat = Payout(A(0), A(50), D::STRIKE, Q16(1), IM, false);
    BOOST_CHECK_EQUAL(flat.payout_owner, IM);
}

// ---- Dust-snap ------------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(dust_snaps_subdust_cp_leg_to_zero)
{
    // f_loss = 1*(1000000-999999)/1000000 -> cp = 1 sat (< MIN) -> snapped to 0, owner keeps all.
    const auto p = Payout(A(1'000'000), A(999'999), D::STRIKE, Q16(1), IM, false);
    BOOST_CHECK_EQUAL(p.payout_cp, 0u);
    BOOST_CHECK_EQUAL(p.payout_owner, IM);
}

BOOST_AUTO_TEST_CASE(dust_snaps_subdust_owner_leg_to_zero)
{
    // cp = 999900, owner = 100 (< MIN) -> owner snapped to 0, cp takes the full margin.
    const auto p = Payout(A(1'000'000), A(100), D::STRIKE, Q16(1), IM, false);
    BOOST_CHECK_EQUAL(p.payout_owner, 0u);
    BOOST_CHECK_EQUAL(p.payout_cp, IM);
}

// ---- Truncation (single floor, remainder to owner) ------------------------------------------

BOOST_AUTO_TEST_CASE(non_integral_floor_truncates_remainder_to_owner)
{
    // f_loss = 1 * (3-2)/3 = 1/3 -> cp = floor(1000000/3) = 333333; the fractional remainder
    // (the .333...) accrues to the owner (single floor), so owner = 666667.
    const auto p = Payout(A(3), A(2), D::STRIKE, Q16(1), IM, false);
    BOOST_CHECK_EQUAL(p.payout_cp, 333'333u);
    BOOST_CHECK_EQUAL(p.payout_owner, 666'667u);
}

// ---- Invalid terms --------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(rejects_zero_lambda)
{
    ScalarCfdPayout out;
    BOOST_CHECK(!ComputeScalarCfdPayout(A(100), A(90), D::STRIKE, /*lambda_q=*/0, IM, false, MIN, out));
}

BOOST_AUTO_TEST_CASE(rejects_vault_im_below_min)
{
    ScalarCfdPayout out;
    BOOST_CHECK(!ComputeScalarCfdPayout(A(100), A(90), D::STRIKE, Q16(1), MIN - 1, false, MIN, out));
}

BOOST_AUTO_TEST_CASE(rejects_unknown_denominator_mode)
{
    // Consensus math fails closed on an out-of-range denominator enum (e.g. a future mode 2
    // smuggled past the leaf parser) rather than silently settling as STRIKE.
    ScalarCfdPayout out;
    BOOST_CHECK(!ComputeScalarCfdPayout(A(100), A(90), static_cast<ScalarLossDenominator>(2),
                                        Q16(1), IM, false, MIN, out));
}

// ---- Parity with the difficulty payout (mode 1 reproduces difficulty semantics) -------------
//
// Difficulty's loss is in inverted TARGET space, so a difficulty short_leg (loses as the target
// falls, realized<strike) corresponds to a scalar LONG (loses as X falls below K) with the same
// |X-K| and denom=realized. Likewise difficulty long_leg <-> scalar short. The payout math is then
// byte-identical.

BOOST_AUTO_TEST_CASE(parity_difficulty_short_leg_equals_scalar_long_realized)
{
    DiffCfdPayout d;
    BOOST_REQUIRE(ComputeDiffCfdPayout(/*strike_target=*/A(1000), /*realized_target=*/A(800),
                                       Q16(1), IM, /*short_leg=*/true, d));
    const auto s = Payout(/*K=*/A(1000), /*X=*/A(800), D::REALIZED, Q16(1), IM, /*short_leg=*/false);
    BOOST_CHECK_EQUAL(s.payout_owner, d.payout_owner);
    BOOST_CHECK_EQUAL(s.payout_cp, d.payout_cp);
}

BOOST_AUTO_TEST_CASE(parity_difficulty_long_leg_equals_scalar_short_realized)
{
    DiffCfdPayout d;
    BOOST_REQUIRE(ComputeDiffCfdPayout(/*strike_target=*/A(1000), /*realized_target=*/A(1200),
                                       Q16(1), IM, /*short_leg=*/false, d));
    const auto s = Payout(/*K=*/A(1000), /*X=*/A(1200), D::REALIZED, Q16(1), IM, /*short_leg=*/true);
    BOOST_CHECK_EQUAL(s.payout_owner, d.payout_owner);
    BOOST_CHECK_EQUAL(s.payout_cp, d.payout_cp);
}

// ---- Large-value 512-bit envelope -----------------------------------------------------------

BOOST_AUTO_TEST_CASE(large_scalar_no_overflow)
{
    // Near-2^256 strike/realized with max lambda and a 2^63-ish vault: exercises the wide
    // accumulator. f_loss = 1 * (X-K)/X with X = 2*K -> 0.5 -> cp = vault/2.
    const arith_uint256 k = (arith_uint256(1) << 200);
    const arith_uint256 x = (arith_uint256(1) << 201); // 2*k
    const uint64_t vault = (1ULL << 62);
    const auto p = Payout(k, x, D::REALIZED, Q16(1), vault, /*short_leg=*/true);
    BOOST_CHECK_EQUAL(p.payout_cp, vault / 2);
    BOOST_CHECK_EQUAL(p.payout_owner, vault - vault / 2);
}

BOOST_AUTO_TEST_SUITE_END()
