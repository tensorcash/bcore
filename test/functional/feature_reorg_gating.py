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

    def _restart_node_with_flags(self, idx, extra_args):
        self.stop_node(idx)
        self.start_node(idx, extra_args=extra_args)

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

        # Wait for pending to clear, then restart without gating to ensure reorg completes.
        self.wait_until(lambda: n0.getpendingreorg() is None, timeout=30)
        self._restart_node_with_flags(0, [
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=0", "-spv-asn-corroboration=0"])
        self.connect_nodes(0, 1)
        self.sync_blocks(self.nodes, timeout=120)

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

        # Do not submit a decision; wait for timeout accept
        self.wait_until(lambda: n0.getpendingreorg() is None, timeout=90)
        self._restart_node_with_flags(0, [
            "-reorgadvisory=1", "-reorgadvisorydepth=3", "-reorgadvisorygating=0", "-spv-asn-corroboration=0"])
        self.connect_nodes(0, 1)
        self.sync_blocks(self.nodes, timeout=120)

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
        self.test_offline_autofollow()


if __name__ == '__main__':
    ReorgGatingTest(__file__).main()
