#!/usr/bin/env python3
"""Functional test for consensus-pinned proof window-size enforcement.

The tensor main/test chains pin nEnforcedProofWindowSize=256: QuickVerifier
block sanity rejects any proof whose chosen-token count differs, for every
proof version. Regtest defaults to 0 (unenforced) because this harness mines
deliberately short transcripts; the -enforcedproofwindowsize knob turns the
same consensus rule on so it can be exercised here:

  knob=256 : a short (8-token) v2 proof is rejected up front.
  default  : the same proof mines a block (the verifier adopts the proof's
             own window size, preserving harness behavior).
"""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.vdf_helper import HAS_CHIAVDF

REGTEST_NETWORK = "regtest"
P2_OP_TRUE_HEX = "51"


class QuickVerifyWindowTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-enforcedproofwindowsize=256",
            "-spv-asn-corroboration=0",
        ]]

    def skip_test_if_missing_module(self):
        if not HAS_CHIAVDF:
            raise self.skip_test("requires chiavdf for VDF proofs")
        try:
            import flatbuffers  # noqa: F401
            from test_framework.mining_response_builder import (  # noqa: F401
                build_mining_response, solve_work_unit,
            )
        except ImportError as e:
            raise self.skip_test("requires flatbuffers + mining_response_builder: %s" % e)

    def run_test(self):
        from test_framework.mining_response_builder import (
            build_mining_response, solve_work_unit,
        )
        node = self.nodes[0]
        self._nonce = 0

        registered = [m for m in node.getmodelslist()
                      if m["status"] == 2 and m["difficulty"] > 0]
        assert registered, "node has no registered model to mine against"
        model_id = "%s@%s" % (registered[0]["model_name"], registered[0]["model_commit"])

        def mine(num_tokens=8):
            self._nonce += 1
            unit = node.create_mining_work_unit(
                REGTEST_NETWORK, P2_OP_TRUE_HEX, "%02x" % (self._nonce & 0xff))
            sol = solve_work_unit(unit["header_prefix"], unit["target"],
                                  num_tokens=num_tokens)
            payload = build_mining_response(unit["req_id"], sol,
                                            model_identifier=model_id, version=2)
            return node.submit_mining_response(unit["req_id"], payload)

        # --- enforced: a short proof must be rejected up front ---
        self.log.info("-enforcedproofwindowsize=256: 8-token proof rejected")
        res = mine(num_tokens=8)
        assert_equal(res["accepted"], False)
        assert_equal(node.getblockcount(), 0)
        self.log.info("  reject reason: %s" % res.get("reject_reason", ""))

        # --- default (unenforced): the same short proof mines a block ---
        self.log.info("default (0): the same 8-token proof is accepted")
        self.restart_node(0, extra_args=["-spv-asn-corroboration=0"])
        res = mine(num_tokens=8)
        assert_equal(res["accepted"], True)
        assert_equal(node.getblockcount(), 1)

        self.log.info("consensus-pinned window-size enforcement verified")


if __name__ == "__main__":
    QuickVerifyWindowTest(__file__).main()
