#!/usr/bin/env python3
# Copyright (c) 2024-present The TensorCash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test reorg advisory gating flow (operator decisions, timeout, offline guard)."""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)


class ReorgGatingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=1",
             "-reorgadvisorygatingdepth=3", "-reorgadvisorytimeout=5", "-spv-asn-corroboration=0"],
            ["-reorgadvisory=1", "-reorgadvisorydepth=3", "-spv-asn-corroboration=0"],
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def no_op(self):
        pass

    def _build_divergent_chains(self, blocks_a, blocks_b):
        """Disconnect, mine diverging branches, then reconnect."""
        n0, n1 = self.nodes
        self.disconnect_nodes(0, 1)
        self.generate(n0, blocks_a, sync_fun=self.no_op)
        self.generate(n1, blocks_b, sync_fun=self.no_op)
        self.connect_nodes(0, 1)

    def _wait_for_pending(self, node):
        self.wait_until(lambda: node.getpendingreorg() is not None, timeout=10)
        return node.getpendingreorg()

    def _clear_pending_if_any(self, node):
        pending = node.getpendingreorg()
        if pending is not None:
            node.submitreorgdecision("accept")
            self.wait_until(lambda: node.getpendingreorg() is None, timeout=10)

    def test_reject_path(self):
        self.log.info("Testing gating reject path")
        n0, n1 = self.nodes

        self.generate(n0, 110)
        self.sync_all()

        # Diverge: node0 shorter (4), node1 longer (7)
        self._build_divergent_chains(4, 7)

        pending = self._wait_for_pending(n0)
        assert_greater_than(pending["depth_current"], 3)

        # Reject the reorg
        res = n0.submitreorgdecision("reject")
        assert res["success"]

        time.sleep(1)
        # Node0 should stay on its shorter chain
        assert n0.getblockcount() < n1.getblockcount()

    def test_accept_path(self):
        self.log.info("Testing gating accept path")
        n0, n1 = self.nodes

        # First sync both nodes without gating to avoid pending reorgs blocking sync_all.
        self.restart_node(0, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=0", "-spv-asn-corroboration=0"])
        self.restart_node(1, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-spv-asn-corroboration=0"])
        self.connect_nodes(0, 1)
        self._clear_pending_if_any(n0)
        self.sync_all()

        self.generate(n0, 110)
        self.sync_all()

        # Now enable gating on node0 only for the divergent reorg scenario.
        self.restart_node(0, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=1",
            "-reorgadvisorygatingdepth=3", "-reorgadvisorytimeout=5", "-spv-asn-corroboration=0"])
        self.connect_nodes(0, 1)
        self.sync_all()

        self._build_divergent_chains(4, 7)
        pending = self._wait_for_pending(n0)
        assert_greater_than(pending["depth_current"], 3)

        res = n0.submitreorgdecision("accept")
        assert res["success"]

        # A single accept is recorded as a durable approval anchored to the
        # reorg's fork point, so the whole reorg must complete with gating
        # still enabled - no second prompt, no restart workaround.
        self.wait_until(lambda: n0.getpendingreorg() is None, timeout=30)
        self.sync_blocks(self.nodes, timeout=120)
        assert n0.getpendingreorg() is None

    def test_timeout_accept(self):
        self.log.info("Testing gating timeout accept path")
        n0, n1 = self.nodes
        self.restart_node(0, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=0", "-spv-asn-corroboration=0"])
        self.restart_node(1, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-spv-asn-corroboration=0"])
        self.connect_nodes(0, 1)
        self._clear_pending_if_any(n0)
        self.sync_all()

        self.generate(n0, 110)
        self.sync_all()

        self.restart_node(0, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=1",
            "-reorgadvisorygatingdepth=3", "-reorgadvisorytimeout=5",
            "-reorgadvisorytimeoutaccept=1", "-spv-asn-corroboration=0"])
        self.connect_nodes(0, 1)
        self.sync_all()

        self._build_divergent_chains(4, 7)
        pending = self._wait_for_pending(n0)
        assert_greater_than(pending["depth_current"], 3)

        # Do not submit a decision; wait for timeout accept. A timeout-accept
        # records the same durable approval as an operator accept, so the
        # reorg must complete with gating still enabled.
        self.wait_until(lambda: n0.getpendingreorg() is None, timeout=90)
        self.sync_blocks(self.nodes, timeout=120)
        assert n0.getpendingreorg() is None

    def test_operator_rpcs_not_gated(self):
        self.log.info("Testing invalidateblock/reconsiderblock bypass the gate")
        n0, n1 = self.nodes

        # Sync both nodes with gating off, then diverge: branch A mined by n0
        # (longer, active) and branch B mined by n1, submitted to n0 as a side
        # branch so its arrival does not itself trigger a reorg.
        self.restart_node(0, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=0", "-spv-asn-corroboration=0"])
        self.restart_node(1, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-spv-asn-corroboration=0"])
        self.connect_nodes(0, 1)
        self._clear_pending_if_any(n0)
        self.sync_all()
        self.generate(n0, 10)
        self.sync_all()

        self.disconnect_nodes(0, 1)
        a_blocks = self.generate(n0, 9, sync_fun=self.no_op)
        b_blocks = self.generate(n1, 6, sync_fun=self.no_op)
        for bh in b_blocks:
            n0.submitblock(n1.getblock(bh, 0))
        a_tip = a_blocks[-1]
        b_tip = b_blocks[-1]
        assert_equal(n0.getbestblockhash(), a_tip)

        # Enable gating with a long timeout: if the operator RPCs were routed
        # through the gate they would block until this timeout.
        self.restart_node(0, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=1",
            "-reorgadvisorygatingdepth=3", "-reorgadvisorytimeout=600", "-spv-asn-corroboration=0"])

        # Invalidating 4 blocks deep on branch A makes branch B the best
        # chain: a depth-4 branch switch that would gate if peer-initiated.
        start = time.time()
        n0.invalidateblock(a_blocks[4])
        assert time.time() - start < 60
        assert_equal(n0.getbestblockhash(), b_tip)
        assert n0.getpendingreorg() is None

        # Reconsidering switches back to branch A: a depth-6 switch, again
        # operator-initiated and therefore not gated.
        start = time.time()
        n0.reconsiderblock(a_blocks[4])
        assert time.time() - start < 60
        assert_equal(n0.getbestblockhash(), a_tip)
        assert n0.getpendingreorg() is None

    def test_offline_autofollow(self):
        self.log.info("Testing offline >6h auto-follow (no gating)")
        n0, n1 = self.nodes
        # Run this scenario with gating disabled to exercise offline auto-follow deterministically.
        self.restart_node(0, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=0", "-spv-asn-corroboration=0"])
        self.restart_node(1, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-spv-asn-corroboration=0"])
        self.connect_nodes(0, 1)
        self._clear_pending_if_any(n0)
        self.sync_all()

        self.generate(n0, 110)
        self.sync_all()

        # Advance mock time by 7 hours to exceed offline threshold
        now = int(time.time())
        n0.setmocktime(now + 7 * 60 * 60)
        n1.setmocktime(now + 7 * 60 * 60)

        self._build_divergent_chains(4, 7)

        # Chain should auto-reorg; allow generous time for block sync.
        self.sync_blocks(self.nodes, timeout=120)
        assert n0.getpendingreorg() is None

    # ------------------------------------------------------------------
    # Non-blocking mask-mode scenarios (§3.1 test matrix)
    # ------------------------------------------------------------------

    MASK_ARGS = [
        "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=1",
        "-reorgadvisorygatingdepth=3", "-reorgadvisorygatingmode=mask",
        "-reorgadvisorytimeout=600", "-reorgadvisoryvetottl=600",
        "-reorgadvisoryvetogrowth=2", "-checkblockindex=1",
        "-spv-asn-corroboration=0",
    ]

    def _resync_nodes(self):
        """Restart both nodes gating-off, reconnect and fully sync them."""
        self.restart_node(0, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=0",
            "-checkblockindex=1", "-spv-asn-corroboration=0"])
        self.restart_node(1, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-spv-asn-corroboration=0"])
        self.connect_nodes(0, 1)
        self._clear_pending_if_any(self.nodes[0])
        self.sync_all()

    def _arm_mask_gate(self, extra_args=None):
        """Sync, enable mask gating on node0, diverge 4 vs 7 and wait for the gate."""
        n0, n1 = self.nodes
        self._resync_nodes()
        self.generate(n0, 20)
        self.sync_all()
        self.restart_node(0, extra_args=(extra_args or self.MASK_ARGS))
        self.connect_nodes(0, 1)
        self.sync_all()
        self._build_divergent_chains(4, 7)
        return self._wait_for_pending(n0)

    def test_mask_liveness_while_gated(self):
        self.log.info("Testing mask mode: node stays fully live while a gate is pending")
        n0, n1 = self.nodes
        self._arm_mask_gate()
        n0_tip_before = n0.getbestblockhash()
        pending = n0.getpendingreorg()
        gate = pending["gates"][0]
        assert_equal(gate["state"], "pending")
        anchor = gate["anchor"]

        # Block submission while gated: mining on the current tip must succeed
        # promptly (the old blocking gate hung submitblock/ProcessNewBlock).
        start = time.time()
        mined = self.generate(n0, 1, sync_fun=self.no_op)
        assert time.time() - start < 60
        assert_equal(n0.getbestblockhash(), mined[-1])
        # The gate is still armed and the node did not follow the candidate.
        assert n0.getpendingreorg() is not None

        # invalidateblock while gated must not block: emergency-kill the
        # candidate subtree at its anchor.
        start = time.time()
        n0.invalidateblock(anchor)
        assert time.time() - start < 60
        assert_equal(n0.getbestblockhash(), mined[-1])

        # reconsiderblock while gated must not block either. The operator RPC
        # carries sign-off, so it may switch to the (heavier) candidate branch
        # without re-prompting - same override semantics as
        # test_operator_rpcs_not_gated.
        start = time.time()
        n0.reconsiderblock(anchor)
        assert time.time() - start < 60
        assert_equal(n0.getbestblockhash(), n1.getbestblockhash())

    def test_mask_gate_pruned_on_operator_invalidate(self):
        self.log.info("Testing mask mode: raw invalidateblock of the candidate branch prunes the gate immediately")
        n0, n1 = self.nodes
        pending = self._arm_mask_gate()
        anchor = pending["gates"][0]["anchor"]

        # Raw operator invalidateblock (no submitreorgdecision involved) of the
        # candidate subtree anchor: the gate is moot and must disappear
        # immediately - no decision-timeout or veto-TTL wait.
        n0.invalidateblock(anchor)
        assert n0.getpendingreorg() is None

        # Undo for teardown: reconsider restores the branch; the operator RPC
        # may switch to it (heavier) and any re-armed gate whose anchor lands
        # on the active chain is pruned eagerly too.
        n0.reconsiderblock(anchor)
        assert n0.getpendingreorg() is None
        assert_equal(n0.getbestblockhash(), n1.getbestblockhash())

    def test_mask_gate_pruned_on_operator_force_active(self):
        self.log.info("Testing mask mode: reconsiderblock forcing the gated branch active prunes the gate immediately")
        n0, n1 = self.nodes
        pending = self._arm_mask_gate()
        anchor = pending["gates"][0]["anchor"]

        # reconsiderblock on the (not invalid) anchor runs an operator-guarded
        # ActivateBestChain that bypasses the mask and switches to the heavier
        # candidate branch. The gate's anchor is now on the active chain, so
        # the gate must be gone as soon as the RPC returns.
        n0.reconsiderblock(anchor)
        assert_equal(n0.getbestblockhash(), n1.getbestblockhash())
        assert n0.getpendingreorg() is None

    def test_mask_reject_suppression_and_growth_escape(self):
        self.log.info("Testing mask mode: reject vetoes without re-prompt; growth/raw-work escape re-prompts once")
        n0, n1 = self.nodes
        self._arm_mask_gate()

        res = n0.submitreorgdecision("reject")
        assert res["success"]
        gate_id = res["gate_id"]

        # The veto is visible and the node stayed on its own chain.
        pending = n0.getpendingreorg()
        assert_equal(pending["gates"][0]["state"], "vetoed")
        assert n0.getblockcount() < n1.getblockcount()

        # Identical re-prompt suppression: re-evaluations without candidate
        # growth (e.g. mining on our own tip re-runs fork choice) must not
        # resurrect the prompt.
        self.generate(n0, 1, sync_fun=self.no_op)
        time.sleep(2)
        pending = n0.getpendingreorg()
        assert pending is not None
        assert_equal(pending["gates"][0]["state"], "vetoed")
        assert_equal(pending["gates"][0]["gate_id"], gate_id)

        # Growth escape: the vetoed branch extends by >= vetogrowth (2) blocks
        # (its raw-work margin over our tip grows too - either escape is a
        # correct re-prompt trigger). The veto expires early and re-prompts
        # exactly once, with a fresh gate.
        self.generate(n1, 2, sync_fun=self.no_op)
        self.wait_until(
            lambda: n0.getpendingreorg() is not None
            and any(g["state"] == "pending" for g in n0.getpendingreorg()["gates"]),
            timeout=30)
        new_gate = [g for g in n0.getpendingreorg()["gates"] if g["state"] == "pending"][0]
        assert new_gate["gate_id"] != gate_id

        # Accept the re-prompted gate: the branch activates.
        res = n0.submitreorgdecision("accept", new_gate["gate_id"])
        assert res["success"]
        self.wait_until(lambda: n0.getpendingreorg() is None, timeout=30)
        self.sync_blocks(self.nodes, timeout=120)
        assert_equal(n0.getbestblockhash(), n1.getbestblockhash())

    def test_mask_clear_veto_then_accept(self):
        self.log.info("Testing mask mode: clearreorgveto lifts the veto, accept then activates the branch")
        n0, n1 = self.nodes
        self._arm_mask_gate()

        res = n0.submitreorgdecision("reject")
        assert res["success"]
        assert_equal(n0.getpendingreorg()["gates"][0]["state"], "vetoed")

        # Lift the veto: the branch may prompt again (clearreorgveto kicks a
        # re-activation, so no new block is needed).
        res = n0.clearreorgveto()
        assert res["success"]
        self.wait_until(
            lambda: n0.getpendingreorg() is not None
            and any(g["state"] == "pending" for g in n0.getpendingreorg()["gates"]),
            timeout=30)

        # Accept after veto-clear activates the branch.
        res = n0.submitreorgdecision("accept")
        assert res["success"]
        self.wait_until(lambda: n0.getpendingreorg() is None, timeout=30)
        self.sync_blocks(self.nodes, timeout=120)
        assert_equal(n0.getbestblockhash(), n1.getbestblockhash())

    def test_mask_direction_fields(self):
        self.log.info("Testing getpendingreorg direction fields (raw vs effective work)")
        n0, n1 = self.nodes
        pending = self._arm_mask_gate()

        gate = pending["gates"][0]
        # Legacy single-gate mirror stays populated for runbook compatibility.
        assert_equal(pending["pending"], True)
        assert_equal(pending["candidate_tip"], gate["candidate_tip"])
        assert_equal(pending["gate_count"], 1)

        # The 7-block candidate is genuinely heavier in raw work than our
        # 4-block branch and no policy demotion is involved.
        assert int(gate["candidate_raw_work"], 16) > int(gate["current_raw_work"], 16)
        assert_equal(gate["penalty_or_policy_driven"], False)
        assert_equal(gate["candidate_has_data"], True)
        assert_equal(gate["tip_demoted_by_full_red"], False)
        # Our stale tip is off the best-header chain (the candidate is best).
        assert_equal(gate["tip_on_best_header_chain"], False)
        assert_equal(int(gate["current_penalty"], 16), 0)
        assert_equal(int(gate["candidate_penalty"], 16), 0)

        # Clean up: accept and settle both nodes on one chain.
        n0.submitreorgdecision("accept")
        self.wait_until(lambda: n0.getpendingreorg() is None, timeout=30)
        self.sync_blocks(self.nodes, timeout=120)

    def test_block_mode_clean_shutdown_mid_gate(self):
        self.log.info("Testing block mode: clean bounded shutdown while a gate is pending")
        n0, n1 = self.nodes
        block_args = [
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=1",
            "-reorgadvisorygatingdepth=3", "-reorgadvisorygatingmode=block",
            "-reorgadvisorytimeout=600", "-checkblockindex=1",
            "-spv-asn-corroboration=0",
        ]
        self._arm_mask_gate(extra_args=block_args)

        # The gate parks the validation thread (legacy behavior). Shutdown
        # must still complete promptly via the interrupt-aware sliced wait -
        # before this fix it hung for the full 600s decision timeout.
        start = time.time()
        self.stop_node(0)
        assert time.time() - start < 60

        # A restart clears the in-memory gate; the node must NOT have
        # auto-accepted the reorg on the way down.
        self.start_node(0, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=0",
            "-checkblockindex=1", "-spv-asn-corroboration=0"])
        assert n0.getpendingreorg() is None
        assert n0.getblockcount() < n1.getblockcount()

        # Reconnect with gating off and let the nodes settle for teardown.
        self.connect_nodes(0, 1)
        self.sync_blocks(self.nodes, timeout=120)

    def test_block_mode_reject_persists_no_rearm(self):
        self.log.info("Testing block mode: reject persists as a veto - no re-arm, no re-park, node stays live")
        n0, n1 = self.nodes
        # vetogrowth=4: in block mode the gate arms (and parks msghand) at the
        # first winning partial segment - here base+5 of n1's 7-block branch -
        # so the veto's growth baseline is that height and the queued
        # remainder (2 blocks) plus the one block mined below counts as
        # growth 3. That must stay under the escape, or this test would be
        # exercising the designed one-shot growth re-prompt instead of the
        # no-re-arm guarantee.
        block_args = [
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=1",
            "-reorgadvisorygatingdepth=3", "-reorgadvisorygatingmode=block",
            "-reorgadvisorytimeout=600", "-reorgadvisoryvetottl=600",
            "-reorgadvisoryvetogrowth=4", "-checkblockindex=1",
            "-spv-asn-corroboration=0",
        ]
        self._arm_mask_gate(extra_args=block_args)

        res = n0.submitreorgdecision("reject")
        assert res["success"]
        # The parked waiter's epilogue converts the reject into a TTL veto;
        # the gate persists instead of being erased.
        self.wait_until(
            lambda: n0.getpendingreorg() is not None
            and n0.getpendingreorg()["gates"][0]["state"] == "vetoed",
            timeout=30)
        gate_id = n0.getpendingreorg()["gates"][0]["gate_id"]
        assert n0.getblockcount() < n1.getblockcount()

        # One more candidate block (with the queued remainder, growth 3 <
        # vetogrowth 4): before the fix ANY next activation re-selected the
        # branch, re-armed a fresh gate and parked the validation thread for
        # another full decision window - forever.
        self.generate(n1, 1, sync_fun=self.no_op)
        time.sleep(2)
        pending = n0.getpendingreorg()
        assert pending is not None
        assert_equal(pending["gates"][0]["state"], "vetoed")
        assert_equal(pending["gates"][0]["gate_id"], gate_id)

        # The node keeps mining on its own chain with no stall.
        start = time.time()
        mined = self.generate(n0, 1, sync_fun=self.no_op)
        assert time.time() - start < 60
        assert_equal(n0.getbestblockhash(), mined[-1])

        # An explicit accept overrides the veto (block mode included) and
        # activates the branch.
        res = n0.submitreorgdecision("accept")
        assert res["success"]
        self.wait_until(lambda: n0.getpendingreorg() is None, timeout=30)
        self.sync_blocks(self.nodes, timeout=120)
        assert_equal(n0.getbestblockhash(), n1.getbestblockhash())

    def test_offline_bypass_respects_recent_veto(self):
        self.log.info("Testing offline >6h bypass is suppressed for a recently vetoed branch (re-prompt, not silent follow)")
        n0, n1 = self.nodes
        # The chain already contains blocks ~7h in the future (mined by
        # test_offline_autofollow), so every restart from here pins -mocktime.
        t_base = int(time.time()) + 8 * 60 * 60
        self.restart_node(0, extra_args=self.MASK_ARGS + [
            "-reorgadvisoryvetottl=60", "-mocktime=%d" % t_base])
        self.restart_node(1, extra_args=[
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-spv-asn-corroboration=0",
            "-mocktime=%d" % t_base])
        self.connect_nodes(0, 1)
        self._clear_pending_if_any(n0)
        self.sync_all()
        self.generate(n0, 10)
        self.sync_all()

        self._build_divergent_chains(4, 7)
        self._wait_for_pending(n0)
        res = n0.submitreorgdecision("reject")
        assert res["success"]
        assert_equal(n0.getpendingreorg()["gates"][0]["state"], "vetoed")

        # 7 hours later: the tip is "offline"-stale (>6h) and the 60s veto
        # TTL has long expired.
        t_late = t_base + 7 * 60 * 60
        n0.setmocktime(t_late)
        n1.setmocktime(t_late)

        # A new candidate block re-runs fork choice: the expired veto is
        # dropped, but its tombstone suppresses the offline auto-follow
        # bypass - the node must RE-PROMPT, never silently follow the branch
        # the operator vetoed.
        self.generate(n1, 1, sync_fun=self.no_op)
        self.wait_until(
            lambda: n0.getpendingreorg() is not None
            and any(g["state"] == "pending" for g in n0.getpendingreorg()["gates"]),
            timeout=30)
        assert n0.getblockcount() < n1.getblockcount()

        # Accept the re-prompt to settle both nodes for teardown.
        res = n0.submitreorgdecision("accept")
        assert res["success"]
        self.wait_until(lambda: n0.getpendingreorg() is None, timeout=30)
        self.sync_blocks(self.nodes, timeout=120)
        assert_equal(n0.getbestblockhash(), n1.getbestblockhash())

    def run_test(self):
        self.test_reject_path()
        self.test_accept_path()
        self.test_timeout_accept()
        self.test_operator_rpcs_not_gated()
        self.test_mask_liveness_while_gated()
        self.test_mask_gate_pruned_on_operator_invalidate()
        self.test_mask_gate_pruned_on_operator_force_active()
        self.test_mask_reject_suppression_and_growth_escape()
        self.test_mask_clear_veto_then_accept()
        self.test_mask_direction_fields()
        self.test_block_mode_clean_shutdown_mid_gate()
        self.test_block_mode_reject_persists_no_rearm()
        # The remaining tests advance mocktime by hours and mine blocks with
        # future timestamps, after which a plain restart (no -mocktime)
        # refuses to start on the "block from the future" startup check - so
        # they run last, and the second one pins -mocktime on every restart.
        self.test_offline_autofollow()
        self.test_offline_bypass_respects_recent_veto()


if __name__ == '__main__':
    ReorgGatingTest(__file__).main()
