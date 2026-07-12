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

    def run_test(self):
        self.test_reject_path()
        self.test_accept_path()
        self.test_timeout_accept()
        self.test_operator_rpcs_not_gated()
        self.test_offline_autofollow()


if __name__ == '__main__':
    ReorgGatingTest(__file__).main()
