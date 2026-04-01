#!/usr/bin/env python3
# Copyright (c) 2026-present The TensorCash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Build valid proof::MiningResponse FlatBuffers for broker-mining tests.

This is the Python helper the `submit_mining_response` done-when criterion
was gated on (feature_broker_mining_rpc.py). It produces payloads that pass
the FULL acceptance path on regtest:

    submit_mining_response (rpc/mining.cpp)
      -> fillFromFB (primitives/proofblob.cpp)
      -> QuickVerifier::QuickVerify (verification/quick_verifier.cpp)
      -> ProcessNewBlock -> CheckProofOfWork(GetShortHash(), nAdjBits)

The verification contract this mirrors (file references are the source of
truth — re-derive from them if QuickVerifier changes):

- VerifyVDF: real Wesolowski verify via vdf::VerifyAgainstPrevHash with
  challenge = header_prefix[4:36] (raw bytes), iterations = proof.tick,
  discriminant 1024 bits. There is NO regtest bypass, so a real chiavdf
  proof is required (vdf_helper.generate_vdf_proof).
- ComputeUValue / VerifyFinalHash message layout:
      header_prefix(76) || vdf || tick_u32_le || step_u32_le
      || window_tokens(POW_WINDOW_SIZE x int64_le, right-aligned context)
      || compute_precision_bytes
  hashed with single SHA256. Final hash uses step=0 and the full context
  (prompt + chosen; exactly chosen when len(chosen) == POW_WINDOW_SIZE);
  per-step U values use context = prompt + chosen[:step].
- DigestToU: first 4 digest bytes as a little-endian integer, divided by
  2^32, rounded once to float32. We replicate the float32 rounding exactly
  so the stored sampling_u matches the verifier's recomputation bit-for-bit
  (tolerance is 1e-7; ours is 0).
- VerifyHeaderPoW: nonce = first 4 bytes of the final hash;
  dSHA256(header_prefix || hash[:4]) interpreted little-endian must be
  <= target(nAdjBits at header_prefix[72:76]); proof.target must equal the
  32-byte big-endian serialisation of that target (the work unit's `target`
  hex). Regtest nBits 0x207fffff makes a random hash win ~50% of the time,
  so `solve_work_unit` grinds a salt token in chosen_tokens.
- VerifySampleStep: we give the chosen token logit 50.0 and every other
  candidate 0.0 with temperature=1.0, top_p=1.0, repetition_penalty=1.0,
  so after softmax its CDF interval is essentially (0, 1] and any U passes.
- VerifyEntropy: skipped entirely by omitting chosen_probs.
- Model registration: QuickVerifier::VerifyModelRegistration enforces that
  GetModelHash(model_identifier) exists in g_modeldb with difficulty > 0
  whenever g_modeldb is loaded — and on a live regtest bitcoind it IS
  loaded (the "regtest skips the model check" shortcut only holds for unit
  tests that never construct g_modeldb). The DB ships exactly one
  registered model; its (name, commit) are chain-dependent (TensorReg's is
  the empty model → identifier "@"). So callers must pass the identifier of
  an actually-registered model — query getmodelslist and use
  f'{name}@{commit}'. The MODEL_IDENTIFIER default below is only a fallback.
"""

import base64
import hashlib
import struct

import flatbuffers

from test_framework.proof_fb import (
    FloatArray as FA,
    MiningResponse as MR,
    Proof as PF,
    UIntArray as UA,
)
from test_framework.vdf_helper import generate_vdf_proof

# Must match verification/quick_verifier.h
POW_WINDOW_SIZE = 256

# Fallback model identifier. Real callers should pass the identifier of a
# model the node actually has registered (getmodelslist); on TensorReg that
# is the empty model, identifier "@". GetModelHash requires an '@'.
MODEL_IDENTIFIER = "@"
COMPUTE_PRECISION = "fp16"

# Sampling parameters chosen to make VerifySampleStep trivially satisfiable
# while staying inside VerifyParameters' allowed ranges (quick_verifier.h).
TEMPERATURE = 1.0
TOP_P = 1.0
TOP_K = 8
REPETITION_PENALTY = 1.0


def _f32(x):
    """Round a Python float to its float32 value (as a Python float)."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def _digest_to_u(digest):
    """Mirror QuickVerifier::DigestToU (quick_verifier.cpp).

    C++ sums b0 + b1*2^8 + b2*2^16 + b3*2^24 in float32 and divides by 2^32.
    The partial sums up to b2*2^16 are exact integers below 2^24 (exactly
    representable in float32); the only rounding happens adding b3*2^24,
    i.e. a single round-to-nearest of the exact uint32 value. Dividing by
    2^32 is exact. So float32(uint32_value) / 2^32 == float32(uint32_value
    / 2^32), which is what we compute here.
    """
    val = struct.unpack("<I", digest[:4])[0]
    return _f32(val / 4294967296.0)


def _pow_message(header_prefix, vdf, tick, step, context):
    """The SHA256 preimage shared by ComputeUValue and VerifyFinalHash."""
    msg = bytearray()
    msg += header_prefix
    msg += vdf
    msg += struct.pack("<I", tick & 0xFFFFFFFF)  # C++ truncates tick to u32
    msg += struct.pack("<I", step)
    window = [0] * POW_WINDOW_SIZE
    ctx_len = min(len(context), POW_WINDOW_SIZE)
    if ctx_len:
        window[POW_WINDOW_SIZE - ctx_len:] = context[-ctx_len:]
    for token in window:
        msg += struct.pack("<q", token)
    msg += COMPUTE_PRECISION.encode()
    return bytes(msg)


def compute_final_hash(header_prefix, vdf, tick, prompt_tokens, chosen_tokens):
    """Mirror QuickVerifier::VerifyFinalHash's recomputation (step=0)."""
    if len(chosen_tokens) == POW_WINDOW_SIZE:
        ctx = list(chosen_tokens)
    else:
        ctx = list(prompt_tokens) + list(chosen_tokens)
    return hashlib.sha256(_pow_message(header_prefix, vdf, tick, 0, ctx)).digest()


def compute_sampling_u(header_prefix, vdf, tick, prompt_tokens, chosen_tokens):
    """Mirror QuickVerifier::GetUValues: context excludes the current step."""
    us = []
    for step in range(len(chosen_tokens)):
        ctx = list(prompt_tokens) + list(chosen_tokens[:step])
        digest = hashlib.sha256(
            _pow_message(header_prefix, vdf, tick, step, ctx)
        ).digest()
        us.append(_digest_to_u(digest))
    return us


def _dsha256(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def solve_work_unit(header_prefix_hex, target_hex, *, tick=100000,
                    num_tokens=8, want_winner=True, max_attempts=4096):
    """Solve (or deliberately lose) a create_mining_work_unit work unit.

    Grinds the last chosen token until dSHA256(header_prefix || hash[:4])
    is <= target (want_winner=True) or > target (want_winner=False — used
    to exercise the quick_verify_failed path with an otherwise well-formed
    proof). Regtest's 0x207fffff target wins ~50% per attempt, so both
    grinds converge in a couple of iterations.

    Returns a dict consumed by build_mining_response().
    """
    header_prefix = bytes.fromhex(header_prefix_hex)
    assert len(header_prefix) == 76, len(header_prefix)
    target_int = int(target_hex, 16)
    target_bytes = bytes.fromhex(target_hex)
    assert len(target_bytes) == 32, len(target_bytes)

    # VerifyVDF's challenge is the raw prev-hash bytes from the prefix.
    prev_hash = header_prefix[4:36]
    vdf = generate_vdf_proof(prev_hash, tick)

    adjusted_bits = struct.unpack("<I", header_prefix[72:76])[0]

    base_tokens = [101 + i for i in range(num_tokens - 1)]
    for salt in range(max_attempts):
        chosen_tokens = base_tokens + [salt]
        final_hash = compute_final_hash(header_prefix, vdf, tick, [], chosen_tokens)
        header_hash = _dsha256(header_prefix + final_hash[:4])
        # uint256 stores little-endian; UintToArith256 compares numerically.
        is_winner = int.from_bytes(header_hash, "little") <= target_int
        if is_winner == want_winner:
            return {
                "header_prefix": header_prefix,
                "target": target_bytes,
                "vdf": vdf,
                "tick": tick,
                "chosen_tokens": chosen_tokens,
                "final_hash": final_hash,
                "sampling_u": compute_sampling_u(
                    header_prefix, vdf, tick, [], chosen_tokens
                ),
                "nonce": struct.unpack("<I", final_hash[:4])[0],
                "adjusted_bits": adjusted_bits,
            }
    raise RuntimeError(
        f"no {'winning' if want_winner else 'losing'} salt in "
        f"{max_attempts} attempts — target {target_hex} is degenerate?"
    )


def build_mining_response(req_id, solution, completion_id=None,
                          model_identifier=MODEL_IDENTIFIER, version=1):
    """Serialise a solved work unit into a base64 proof::MiningResponse.

    model_identifier must name a model registered on the target node
    (getmodelslist) or QuickVerifier::VerifyModelRegistration rejects the
    submission as quick_verify_failed.

    version sets proof.version: < REUSE_GATE_VERSION (2) is legacy/grandfathered,
    >= 2 is gated by the q32 reuse-entropy rule. (greedy proofs from this builder
    have E_reuse == #steps, so num_tokens controls whether a v2 proof passes.)
    """
    builder = flatbuffers.Builder(8192)

    chosen_tokens = solution["chosen_tokens"]
    n = len(chosen_tokens)

    # Per-step top-k candidates: the chosen token gets logit 50.0, the
    # filler candidates 0.0 — softmax gives the chosen token essentially
    # the whole CDF, so VerifySampleStep passes for any U value. Filler
    # token ids start at 2^31 so they can never collide with chosen ids.
    fillers = [0x80000000 + j for j in range(TOP_K - 1)]
    logits_row = [50.0] + [0.0] * (TOP_K - 1)

    fa_offsets = []
    ua_offsets = []
    for step in range(n):
        indices_row = [chosen_tokens[step]] + fillers

        FA.StartValuesVector(builder, TOP_K)
        for v in reversed(logits_row):
            builder.PrependFloat32(v)
        vals = builder.EndVector()
        FA.Start(builder)
        FA.AddValues(builder, vals)
        fa_offsets.append(FA.End(builder))

        UA.StartValuesVector(builder, TOP_K)
        for v in reversed(indices_row):
            builder.PrependUint32(v)
        vals = builder.EndVector()
        UA.Start(builder)
        UA.AddValues(builder, vals)
        ua_offsets.append(UA.End(builder))

    PF.StartTopkLogitsVector(builder, n)
    for off in reversed(fa_offsets):
        builder.PrependUOffsetTRelative(off)
    topk_logits_off = builder.EndVector()

    PF.StartTopkIndicesVector(builder, n)
    for off in reversed(ua_offsets):
        builder.PrependUOffsetTRelative(off)
    topk_indices_off = builder.EndVector()

    PF.StartChosenTokensVector(builder, n)
    for v in reversed(chosen_tokens):
        builder.PrependUint32(v)
    chosen_tokens_off = builder.EndVector()

    PF.StartSamplingUVector(builder, n)
    for v in reversed(solution["sampling_u"]):
        builder.PrependFloat32(v)
    sampling_u_off = builder.EndVector()

    target_off = builder.CreateByteVector(solution["target"])
    vdf_off = builder.CreateByteVector(solution["vdf"])
    hash_off = builder.CreateByteVector(solution["final_hash"])
    block_hash_off = builder.CreateByteVector(solution["header_prefix"][4:36])
    header_prefix_off = builder.CreateByteVector(solution["header_prefix"])
    model_id_off = builder.CreateString(model_identifier)
    precision_off = builder.CreateString(COMPUTE_PRECISION)

    PF.Start(builder)
    PF.AddVersion(builder, version)
    PF.AddTick(builder, solution["tick"])
    PF.AddTimestamp(builder, 1700000000)
    PF.AddTarget(builder, target_off)
    PF.AddVdf(builder, vdf_off)
    PF.AddHash(builder, hash_off)
    PF.AddBlockHash(builder, block_hash_off)
    PF.AddHeaderPrefix(builder, header_prefix_off)
    PF.AddIsSolution(builder, True)
    PF.AddModelIdentifier(builder, model_id_off)
    PF.AddComputePrecision(builder, precision_off)
    PF.AddTemperature(builder, TEMPERATURE)
    PF.AddTopP(builder, TOP_P)
    PF.AddTopK(builder, TOP_K)
    PF.AddRepetitionPenalty(builder, REPETITION_PENALTY)
    PF.AddChosenTokens(builder, chosen_tokens_off)
    PF.AddSamplingU(builder, sampling_u_off)
    PF.AddTopkLogits(builder, topk_logits_off)
    PF.AddTopkIndices(builder, topk_indices_off)
    # chosen_probs deliberately omitted: VerifyEntropy skips when empty.
    proof_off = PF.End(builder)

    # Only the 32-byte size is validated (kBrokerExpectedHashSize); derive
    # deterministically from the final hash for log readability.
    pow_blob_hash_off = builder.CreateByteVector(
        hashlib.sha256(solution["final_hash"]).digest()
    )
    completion_off = (
        builder.CreateString(completion_id) if completion_id else None
    )

    MR.Start(builder)
    MR.AddReqId(builder, req_id)
    MR.AddNonce(builder, solution["nonce"])
    MR.AddAdjustedBits(builder, solution["adjusted_bits"])
    MR.AddPowBlobHash(builder, pow_blob_hash_off)
    MR.AddDifficulty(builder, solution["adjusted_bits"])
    MR.AddPowBlob(builder, proof_off)
    if completion_off is not None:
        MR.AddCompletionId(builder, completion_off)
    builder.Finish(MR.End(builder))

    return base64.b64encode(bytes(builder.Output())).decode("ascii")
