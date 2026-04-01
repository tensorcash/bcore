// Copyright (c) 2024-present The TensorCash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_VALIDATIONADVISORY_H
#define BITCOIN_VALIDATIONADVISORY_H

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <sync.h>
#include <uint256.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <map>
#include <functional>
#include <string>
#include <vector>

class ChainstateManager;

namespace node {
class BlockManager;
} // namespace node

// Default knobs for advisory/gating.
//! Default reorg depth threshold for triggering advisory.
constexpr int ADVISORY_DEPTH_THRESHOLD = 3;
//! Default offline threshold (seconds) - if exceeded, skip advisory.
constexpr int64_t ADVISORY_OFFLINE_THRESHOLD_SECS = 6 * 60 * 60;  // 6 hours
//! Default calibration window (blocks).
constexpr int ADVISORY_CALIBRATION_WINDOW = 2000;
//! Minimum calibration window size (blocks) to derive sec_per_tick.
constexpr int ADVISORY_CALIBRATION_MIN_WINDOW = 100;
//! Default maximum blocks to read for txid collection to limit disk I/O.
//! For deep reorgs, we sample recent blocks rather than reading all.
//! Can be overridden via -reorgadvisorytxidblocks.
constexpr int DEFAULT_MAX_BLOCKS_FOR_TXIDS = 100;
//! Maximum number of recent advisories to retain for RPC query.
constexpr size_t MAX_STORED_ADVISORIES = 100;

// Forward declarations for argument defaults.
int GetMaxBlocksForTxids();

/**
 * Statistics for a chain segment (between LCA and a tip).
 * Used by the reorg advisory system to compare competing branches.
 */
struct SegmentStats {
    //! Number of blocks in this segment (tip - LCA).
    int block_count{0};

    //! Cumulative chainwork difference: tip.nChainWork - lca.nChainWork
    arith_uint256 work_diff{0};

    //! Cumulative VDF tick difference: tip.cumulative_tick - lca.cumulative_tick
    uint64_t ticks_diff{0};

    //! Wall-clock time (seconds) based on first_seen_ts: first_seen(tip) - first_seen(lca)
    //! 0 if either first_seen is unknown.
    int64_t clock_time_secs{0};

    //! Miner-reported time (seconds): tip.nTime - lca.nTime
    int64_t miner_time_secs{0};

    //! Set of non-coinbase transaction IDs in this segment.
    //! Limited to first MAX_BLOCKS_FOR_TXIDS blocks from tip to avoid excessive disk I/O.
    std::set<uint256> txids;

    //! Number of blocks successfully read for txid collection.
    int blocks_read_for_txids{0};

    //! Number of blocks that couldn't be read (pruned or error).
    int blocks_pruned{0};

    //! Whether txid collection was capped due to segment size.
    bool txids_capped{false};

    //! Whether all required data was available to compute this segment.
    bool data_complete{false};

    //! Estimated hashrate (work/time) using clock time. 0 if clock_time <= 0.
    //! Note: Returns relative hashrate units; for percentage comparison use baseline.
    double HashRateFromClock() const {
        if (clock_time_secs <= 0) return 0.0;
        if (work_diff == arith_uint256(0)) return 0.0;
        // Use GetLow64() for approximate comparison - sufficient for relative metrics
        return static_cast<double>(work_diff.GetLow64()) / static_cast<double>(clock_time_secs);
    }

    //! Estimated hashrate using tick-derived time. 0 if ticks_diff <= 0.
    double HashRateFromTicks(double sec_per_tick) const {
        if (sec_per_tick <= 0.0 || ticks_diff == 0) return 0.0;
        double time_from_ticks = static_cast<double>(ticks_diff) * sec_per_tick;
        if (time_from_ticks <= 0.0) return 0.0;
        if (work_diff == arith_uint256(0)) return 0.0;
        return static_cast<double>(work_diff.GetLow64()) / time_from_ticks;
    }
};

/**
 * Calibration data for tick-to-time conversion, computed from a window of blocks.
 */
struct TickTimeCalibration {
    //! Seconds per VDF tick, estimated from historical data.
    double sec_per_tick{0.0};

    //! Baseline network hashrate (work per second) at the LCA.
    double baseline_hashrate{0.0};

    //! Number of blocks used in calibration window.
    int window_blocks{0};

    //! Whether calibration data is reliable.
    bool is_valid{false};

    //! Human-readable reason if not valid.
    std::string invalid_reason;
};

/**
 * Full advisory payload for a deep reorg event.
 */
struct ReorgAdvisory {
    //! Height of the last common ancestor.
    int lca_height{-1};

    //! Reorg depth on current chain (current_tip - lca).
    int depth_current{0};

    //! Reorg depth on new fork (fork_tip - lca).
    int depth_fork{0};

    //! Transaction overlap percentage (Jaccard index: intersection/union * 100).
    double tx_overlap_pct{0.0};

    //! Delay (seconds) before first competing block was seen.
    //! max(first_seen - miner_ts, first_seen - tick_expected_ts)
    int64_t first_block_delay_secs{0};

    //! Estimated hashrate on current segment as % of baseline.
    double hashrate_current_pct{0.0};

    //! Estimated hashrate on fork segment as % of baseline.
    double hashrate_fork_pct{0.0};

    //! Tick-to-time calibration used.
    TickTimeCalibration calibration;

    //! Segment statistics for current chain.
    SegmentStats seg_current;

    //! Segment statistics for new fork.
    SegmentStats seg_fork;

    //! Seconds since we last saw a block (for liveness check).
    int64_t since_last_block_secs{0};

    //! Whether advisory computation succeeded.
    bool is_valid{false};

    //! Human-readable summary for logging.
    std::string Summary() const;
};

/**
 * Block data source interface used by advisory computation.
 * Allows plugging in caching or test fakes without touching ChainstateManager.
 */
class BlockDataSource
{
public:
    virtual ~BlockDataSource() = default;

    /** Read a full block for the given index. */
    virtual bool ReadBlock(const CBlockIndex& index, CBlock& block) const = 0;

    /** Get the first-seen timestamp for a block, or 0 if unknown. */
    virtual int64_t GetFirstSeenTs(const uint256& block_hash) const = 0;
};

/**
 * Cached block data source backed by ChainstateManager/BlockManager.
 * Maintains a small LRU to reduce disk reads during advisory computation.
 */
class CachedBlockDataSource : public BlockDataSource
{
private:
    ChainstateManager& m_chainman;
    const node::BlockManager& m_blockman;
    size_t m_max_entries;
    mutable Mutex m_mutex;
    mutable std::deque<uint256> m_order GUARDED_BY(m_mutex);
    mutable std::map<uint256, CBlock> m_cache GUARDED_BY(m_mutex);

    void TouchEntry(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void Trim() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

public:
    CachedBlockDataSource(ChainstateManager& chainman,
                          const node::BlockManager& blockman,
                          size_t max_entries = 256);

    bool ReadBlock(const CBlockIndex& index, CBlock& block) const override;
    int64_t GetFirstSeenTs(const uint256& block_hash) const override;
};

/**
 * Lambda-backed data source for unit tests.
 */
class LambdaBlockDataSource : public BlockDataSource
{
private:
    std::function<bool(const CBlockIndex&, CBlock&)> m_reader;
    std::function<int64_t(const uint256&)> m_first_seen;

public:
    LambdaBlockDataSource(std::function<bool(const CBlockIndex&, CBlock&)> reader,
                          std::function<int64_t(const uint256&)> first_seen)
        : m_reader(std::move(reader)), m_first_seen(std::move(first_seen)) {}

    bool ReadBlock(const CBlockIndex& index, CBlock& block) const override { return m_reader(index, block); }
    int64_t GetFirstSeenTs(const uint256& block_hash) const override { return m_first_seen(block_hash); }
};

/**
 * Calibrate tick-to-time conversion using a window of blocks before LCA.
 *
 * @param source Block data source with caching.
 * @param lca Last common ancestor block index.
 * @param window_blocks Number of blocks to use for calibration (default 2000).
 * @param min_window Minimum acceptable window size (default ADVISORY_CALIBRATION_MIN_WINDOW).
 * @return Calibration data.
 */
TickTimeCalibration CalibrateTickToTime(
    const BlockDataSource& source,
    const CBlockIndex* lca,
    int window_blocks = ADVISORY_CALIBRATION_WINDOW,
    int min_window = ADVISORY_CALIBRATION_MIN_WINDOW);

/**
 * Compute statistics for a chain segment between LCA and tip.
 *
 * @param source Block data source with caching.
 * @param lca Last common ancestor.
 * @param tip Segment tip.
 * @param sec_per_tick Tick-to-time calibration factor.
 * @param max_blocks_for_txids Limit for txid collection to bound disk I/O.
 * @return Segment statistics.
 */
SegmentStats ComputeSegmentStats(
    const BlockDataSource& source,
    const CBlockIndex* lca,
    const CBlockIndex* tip,
    double sec_per_tick,
    int max_blocks_for_txids = GetMaxBlocksForTxids());

/**
 * Compute transaction overlap between two segments (Jaccard index).
 *
 * @param seg_a First segment stats.
 * @param seg_b Second segment stats.
 * @return Overlap percentage (0-100).
 */
double ComputeTxOverlap(const SegmentStats& seg_a, const SegmentStats& seg_b);

/**
 * Compute the delay before the first competing block was seen.
 *
 * @param source Block data source with caching.
 * @param lca Last common ancestor.
 * @param first_fork_block First block on the fork after LCA.
 * @param sec_per_tick Tick-to-time calibration factor.
 * @return Delay in seconds (max of timestamp-based and tick-based estimates).
 */
int64_t ComputeFirstBlockDelay(
    const BlockDataSource& source,
    const CBlockIndex* lca,
    const CBlockIndex* first_fork_block,
    double sec_per_tick);

/**
 * Generate a full reorg advisory for a proposed chain switch.
 *
 * @param source Block data source (may include caching).
 * @param current_tip Current active chain tip.
 * @param fork_tip Proposed new tip (competing fork).
 * @param lca Last common ancestor.
 * @param calibration_window Window size for sec_per_tick estimation.
 * @param calibration_min_window Minimum acceptable window.
 * @return ReorgAdvisory with all computed metrics.
 */
ReorgAdvisory GenerateReorgAdvisory(
    const BlockDataSource& source,
    const CBlockIndex* current_tip,
    const CBlockIndex* fork_tip,
    const CBlockIndex* lca,
    int calibration_window = ADVISORY_CALIBRATION_WINDOW,
    int calibration_min_window = ADVISORY_CALIBRATION_MIN_WINDOW);

// Convenience wrapper for production callers using ChainstateManager/BlockManager.
ReorgAdvisory GenerateReorgAdvisory(
    ChainstateManager& chainman,
    const node::BlockManager& blockman,
    const CBlockIndex* current_tip,
    const CBlockIndex* fork_tip,
    const CBlockIndex* lca);

/**
 * Compute and log a reorg advisory.
 *
 * Advisory computation happens synchronously (using chainman/blockman references),
 * but logging and storing to the global advisory store happens asynchronously
 * to minimize time holding cs_main.
 *
 * Safe to call while holding cs_main; logging runs in a background task.
 */
void AsyncLogReorgAdvisory(
    ChainstateManager& chainman,
    const node::BlockManager& blockman,
    const CBlockIndex* current_tip,
    const CBlockIndex* fork_tip,
    const CBlockIndex* lca);

/**
 * Configuration for reorg advisory system, read from command-line args.
 */
struct ReorgAdvisoryConfig {
    //! Whether advisory system is enabled.
    bool enabled{true};

    //! Minimum reorg depth to trigger advisory logging.
    int depth_threshold{ADVISORY_DEPTH_THRESHOLD};

    //! Skip advisory if offline longer than this (seconds).
    int64_t offline_threshold_secs{ADVISORY_OFFLINE_THRESHOLD_SECS};
};

/**
 * Get the current reorg advisory configuration.
 * Reads from gArgs on each call (not cached).
 */
ReorgAdvisoryConfig GetReorgAdvisoryConfig();

/**
 * Check if a reorg should trigger advisory logging.
 *
 * @param depth_current Reorg depth on current chain.
 * @param since_last_block Seconds since last block was seen.
 * @param config Optional config; if not provided, reads from gArgs.
 * @return true if advisory should be generated (enabled AND depth > threshold AND since_last < offline_threshold).
 */
bool ShouldTriggerAdvisory(int depth_current, int64_t since_last_block,
                           const std::optional<ReorgAdvisoryConfig>& config = std::nullopt);

/**
 * Thread-safe storage for recent reorg advisories.
 * Stores the most recent MAX_STORED_ADVISORIES entries in a circular buffer.
 */
class ReorgAdvisoryStore
{
private:
    mutable Mutex m_mutex;
    std::deque<ReorgAdvisory> m_advisories GUARDED_BY(m_mutex);

public:
    /**
     * Store a new advisory. If at capacity, removes the oldest entry.
     */
    void Add(const ReorgAdvisory& advisory);

    /**
     * Get the most recent advisory, if any.
     */
    std::optional<ReorgAdvisory> GetLatest() const;

    /**
     * Get the N most recent advisories (newest first).
     * @param count Maximum number to return.
     */
    std::vector<ReorgAdvisory> GetRecent(size_t count) const;

    /**
     * Get all stored advisories (newest first).
     */
    std::vector<ReorgAdvisory> GetAll() const;

    /**
     * Get count of stored advisories.
     */
    size_t Size() const;

    /**
     * Clear all stored advisories.
     */
    void Clear();
};

/**
 * Global store for recent reorg advisories.
 * Used by AsyncLogReorgAdvisory and RPC.
 */
ReorgAdvisoryStore& GetReorgAdvisoryStore();

/**
 * Background worker pool for async advisory logging.
 * Uses a single worker thread with a bounded queue to prevent
 * thread proliferation and ensure clean shutdown.
 */
class AdvisoryWorkerPool
{
private:
    mutable Mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<ReorgAdvisory> m_queue GUARDED_BY(m_mutex);
    std::thread m_worker;
    std::atomic<bool> m_shutdown{false};
    std::atomic<bool> m_started{false};

    //! Maximum queue size to prevent unbounded memory growth.
    static constexpr size_t MAX_QUEUE_SIZE = 100;

    void WorkerLoop();

public:
    AdvisoryWorkerPool() = default;
    ~AdvisoryWorkerPool();

    /**
     * Start the worker thread. Idempotent.
     */
    void Start();

    /**
     * Stop the worker thread and drain the queue.
     * Blocks until worker exits. Called during shutdown.
     */
    void Stop();

    /**
     * Submit an advisory for async logging/storing.
     * If queue is full, drops oldest entry to make room.
     */
    void Submit(ReorgAdvisory advisory);

    /**
     * Check if the worker is running.
     */
    bool IsRunning() const { return m_started.load() && !m_shutdown.load(); }
};

/**
 * Global advisory worker pool instance.
 */
AdvisoryWorkerPool& GetAdvisoryWorkerPool();

/**
 * Stop the advisory worker pool (call during Shutdown).
 */
void StopAdvisoryWorkerPool();

// ============================================================================
// REORG GATING - Operator decision required before chain switch
// ============================================================================

/**
 * Operator decision for a pending reorg.
 */
enum class ReorgDecision {
    NONE,    //!< No decision yet (pending)
    ACCEPT,  //!< Accept the reorg, switch to fork
    REJECT,  //!< Reject the reorg, stay on current chain
    TIMEOUT  //!< Decision timed out, use default action
};

/**
 * State of a pending reorg awaiting operator decision.
 */
struct PendingReorgState {
    //! Whether there's a pending reorg.
    bool is_pending{false};

    //! The advisory for this reorg.
    ReorgAdvisory advisory;

    //! Hash of the candidate (fork) tip.
    uint256 candidate_tip_hash;

    //! Hash of the current tip at time of detection.
    uint256 current_tip_hash;

    //! Timestamp when the pending state was set.
    int64_t pending_since{0};

    //! The operator's decision (NONE while pending).
    ReorgDecision decision{ReorgDecision::NONE};
};

//! Default timeout for operator decision (30 minutes).
constexpr int64_t DEFAULT_REORG_DECISION_TIMEOUT_SECS = 30 * 60;

//! Default action on timeout: true = accept, false = reject.
constexpr bool DEFAULT_REORG_TIMEOUT_ACTION_ACCEPT = false;

//! Default operator gating for deep reorgs.
constexpr bool DEFAULT_REORG_GATING_ENABLED = true;

/**
 * Gating configuration read from command-line args.
 */
struct ReorgGatingConfig {
    //! Whether gating is enabled (-reorgadvisorygating=1).
    bool enabled{DEFAULT_REORG_GATING_ENABLED};

    //! Timeout for operator decision in seconds.
    int64_t timeout_secs{DEFAULT_REORG_DECISION_TIMEOUT_SECS};

    //! Default action on timeout: true = accept fork, false = reject (stay on current).
    bool timeout_accept{DEFAULT_REORG_TIMEOUT_ACTION_ACCEPT};

    //! Minimum depth to trigger gating (can be different from advisory threshold).
    int gating_depth_threshold{ADVISORY_DEPTH_THRESHOLD};
};

/**
 * Get the current reorg gating configuration.
 */
ReorgGatingConfig GetReorgGatingConfig();

/**
 * Thread-safe manager for pending reorg decisions.
 * Coordinates between validation (which blocks) and RPC/UI (which submits decisions).
 */
class ReorgGatingManager
{
private:
    mutable Mutex m_mutex;
    std::condition_variable m_cv;
    PendingReorgState m_pending GUARDED_BY(m_mutex);
    ReorgGatingConfig m_config;

public:
    ReorgGatingManager();

    /**
     * Check if gating is enabled.
     */
    bool IsEnabled() const;

    /**
     * Set a pending reorg awaiting operator decision.
     * Called from ActivateBestChainStep when a deep reorg is detected.
     *
     * @param advisory The computed advisory.
     * @param candidate_tip_hash Hash of the fork tip.
     * @param current_tip_hash Hash of current active tip.
     */
    void SetPending(const ReorgAdvisory& advisory,
                    const uint256& candidate_tip_hash,
                    const uint256& current_tip_hash);

    /**
     * Get the current pending state.
     * Returns a copy to avoid holding the lock.
     */
    PendingReorgState GetPending() const;

    /**
     * Check if there's a pending decision.
     */
    bool HasPending() const;

    /**
     * Submit an operator decision (accept or reject).
     * Wakes up any thread blocked on WaitForDecision.
     *
     * @param decision The decision (ACCEPT or REJECT).
     * @return true if decision was accepted (there was a pending reorg).
     */
    bool SubmitDecision(ReorgDecision decision);

    /**
     * Wait for an operator decision or timeout.
     * Called from ActivateBestChainStep to block until decision.
     *
     * @return The decision (ACCEPT, REJECT, or TIMEOUT).
     */
    ReorgDecision WaitForDecision();

    /**
     * Clear the pending state.
     * Called after the decision has been processed.
     */
    void ClearPending();

    /**
     * Reload configuration from gArgs.
     */
    void ReloadConfig();

    /**
     * Get remaining time until timeout (seconds).
     * Returns 0 if no pending or already timed out.
     */
    int64_t GetTimeoutRemaining() const;
};

/**
 * Global reorg gating manager instance.
 */
ReorgGatingManager& GetReorgGatingManager();

/**
 * Check if a reorg should be gated (require operator decision).
 *
 * @param depth_current Reorg depth on current chain.
 * @param since_last_block Seconds since last block was seen.
 * @param disconnect_only True when the candidate chain is an ancestor of the current tip.
 * @param config Optional config; if not provided, reads from gArgs.
 * @return true if gating is enabled and thresholds are met.
 */
bool ShouldGateReorg(int depth_current, int64_t since_last_block, bool disconnect_only = false,
                     const std::optional<ReorgGatingConfig>& config = std::nullopt);

#endif // BITCOIN_VALIDATIONADVISORY_H
