// Copyright (c) 2024-present The TensorCash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <validationadvisory.h>

#include <chain.h>
#include <common/args.h>
#include <common/system.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <util/chaintype.h>
#include <util/string.h>
#include <util/threadnames.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <cmath>
#include <thread>
#include <map>

using util::ReplaceAll;

// ---------------------------------------------------------------------------
// Block data sources
// ---------------------------------------------------------------------------

CachedBlockDataSource::CachedBlockDataSource(ChainstateManager& chainman,
                                             const node::BlockManager& blockman,
                                             size_t max_entries)
    : m_chainman(chainman), m_blockman(blockman), m_max_entries(max_entries) {}

void CachedBlockDataSource::TouchEntry(const uint256& hash) const
{
    // Move hash to front of order deque
    for (auto it = m_order.begin(); it != m_order.end(); ++it) {
        if (*it == hash) {
            m_order.erase(it);
            break;
        }
    }
    m_order.push_front(hash);
}

void CachedBlockDataSource::Trim() const
{
    while (m_cache.size() > m_max_entries && !m_order.empty()) {
        const uint256& evict = m_order.back();
        m_cache.erase(evict);
        m_order.pop_back();
    }
}

bool CachedBlockDataSource::ReadBlock(const CBlockIndex& index, CBlock& block) const
{
    const uint256 hash = index.GetBlockHash();
    {
        LOCK(m_mutex);
        auto it = m_cache.find(hash);
        if (it != m_cache.end()) {
            block = it->second;
            TouchEntry(hash);
            return true;
        }
    }

    if (!m_blockman.ReadBlock(block, index)) {
        return false;
    }

    {
        LOCK(m_mutex);
        m_cache.emplace(hash, block);
        TouchEntry(hash);
        Trim();
    }
    return true;
}

int64_t CachedBlockDataSource::GetFirstSeenTs(const uint256& block_hash) const
{
    return m_blockman.GetBlockFirstSeenTs(block_hash);
}

std::string ReorgAdvisory::Summary() const
{
    if (!is_valid) {
        return "ReorgAdvisory: invalid/incomplete data";
    }

    // Build data quality suffix
    std::string data_notes;
    if (seg_current.txids_capped || seg_fork.txids_capped) {
        data_notes += " [txids_capped]";
    }
    int total_pruned = seg_current.blocks_pruned + seg_fork.blocks_pruned;
    if (total_pruned > 0) {
        data_notes += strprintf(" [%d_blocks_pruned]", total_pruned);
    }

    return strprintf(
        "ReorgAdvisory: LCA=%d, depth_cur=%d, depth_fork=%d, "
        "tx_overlap=%.1f%%, first_block_delay=%llds, "
        "hashrate_cur=%.1f%%, hashrate_fork=%.1f%%, "
        "sec_per_tick=%.6f, calibration_ok=%s%s",
        lca_height, depth_current, depth_fork,
        tx_overlap_pct, first_block_delay_secs,
        hashrate_current_pct, hashrate_fork_pct,
        calibration.sec_per_tick,
        calibration.is_valid ? "true" : "false",
        data_notes
    );
}

TickTimeCalibration CalibrateTickToTime(
    const BlockDataSource& source,
    const CBlockIndex* lca,
    int window_blocks,
    int min_window)
{
    TickTimeCalibration result;
    result.window_blocks = window_blocks;

    if (!lca) {
        result.invalid_reason = "LCA is null";
        return result;
    }

    // Try progressively smaller windows if blocks are pruned
    int try_window = window_blocks;

    while (try_window >= min_window) {
        const CBlockIndex* end_block = lca;
        const CBlockIndex* start_block = lca;
        int actual_window = 0;

        // Walk back try_window blocks from LCA
        for (int i = 0; i < try_window && start_block->pprev; ++i) {
            start_block = start_block->pprev;
            ++actual_window;
        }

        if (actual_window < min_window) {
            result.invalid_reason = strprintf("Window too small: %d blocks", actual_window);
            return result;
        }

        result.window_blocks = actual_window;

        // Try to read blocks - if pruned, reduce window
        CBlock end_blk, start_blk;
        if (!source.ReadBlock(*end_block, end_blk)) {
            // End block pruned - can't calibrate
            result.invalid_reason = "End block (LCA) pruned";
            return result;
        }

        if (!source.ReadBlock(*start_block, start_blk)) {
            // Start block pruned - try smaller window
            try_window /= 2;
            continue;
        }

        // Compute tick difference
        if (end_blk.cumulative_tick <= start_blk.cumulative_tick) {
            result.invalid_reason = "Non-positive tick difference";
            return result;
        }
        uint64_t ticks_diff = end_blk.cumulative_tick - start_blk.cumulative_tick;

        // Compute time difference from miner timestamps (always available from CBlockIndex)
        int64_t time_diff = static_cast<int64_t>(end_block->nTime) - static_cast<int64_t>(start_block->nTime);
        if (time_diff <= 0) {
            result.invalid_reason = "Non-positive time difference";
            return result;
        }

        // Compute sec_per_tick
        result.sec_per_tick = static_cast<double>(time_diff) / static_cast<double>(ticks_diff);

        // Compute baseline hashrate (work per second)
        // nChainWork is always available from CBlockIndex, no disk read needed
        arith_uint256 work_diff = end_block->nChainWork - start_block->nChainWork;
        if (work_diff == arith_uint256(0)) {
            result.invalid_reason = "Zero work difference";
            return result;
        }

        // Convert work to double for hashrate calculation (use full precision).
        double work_approx = work_diff.getdouble();
        result.baseline_hashrate = work_approx / static_cast<double>(time_diff);

        result.is_valid = true;
        return result;
    }

    result.invalid_reason = "All calibration windows pruned";
    return result;
}

SegmentStats ComputeSegmentStats(
    const BlockDataSource& source,
    const CBlockIndex* lca,
    const CBlockIndex* tip,
    double sec_per_tick,
    int max_blocks_for_txids)
{
    SegmentStats result;

    if (!lca || !tip) {
        return result;
    }

    // Compute block count
    result.block_count = tip->nHeight - lca->nHeight;
    if (result.block_count <= 0) {
        return result;
    }

    // Compute work difference
    result.work_diff = tip->nChainWork - lca->nChainWork;

    // Compute miner time difference
    result.miner_time_secs = static_cast<int64_t>(tip->nTime) - static_cast<int64_t>(lca->nTime);

    // Get first_seen timestamps
    int64_t lca_first_seen = source.GetFirstSeenTs(lca->GetBlockHash());
    int64_t tip_first_seen = source.GetFirstSeenTs(tip->GetBlockHash());

    if (lca_first_seen > 0 && tip_first_seen > 0) {
        result.clock_time_secs = tip_first_seen - lca_first_seen;
    }

    // Read LCA block for cumulative_tick (baseline)
    CBlock lca_blk;
    bool have_lca = source.ReadBlock(*lca, lca_blk);

    // Collect transaction IDs from blocks in the segment.
    // Limit to configurable max to avoid excessive disk I/O on deep reorgs.
    // For very deep reorgs, we sample the most recent blocks which are most
    // likely to be unpruned and most relevant for overlap analysis.
    //
    // We read tip first for cumulative_tick, then reuse it for txids to avoid
    // reading the same block twice.
    const CBlockIndex* pindex = tip;
    int blocks_attempted = 0;
    bool have_tip = false;
    uint64_t tip_cumulative_tick = 0;

    while (pindex && pindex != lca && (max_blocks_for_txids == 0 || blocks_attempted < max_blocks_for_txids)) {
        CBlock blk;
        if (source.ReadBlock(*pindex, blk)) {
            // Capture tip's cumulative_tick on first iteration
            if (pindex == tip) {
                have_tip = true;
                tip_cumulative_tick = blk.cumulative_tick;
            }

            // Collect non-coinbase txids
            for (size_t i = 1; i < blk.vtx.size(); ++i) {
                result.txids.insert(blk.vtx[i]->GetHash());
            }
            ++result.blocks_read_for_txids;
        } else {
            // Block is pruned or couldn't be read - track but continue
            if (pindex == tip) {
                // Tip is pruned - can't get tick data
                have_tip = false;
            }
            ++result.blocks_pruned;
        }
        pindex = pindex->pprev;
        ++blocks_attempted;
    }

    // Compute ticks_diff if we have both endpoints
    if (have_lca && have_tip) {
        result.ticks_diff = tip_cumulative_tick - lca_blk.cumulative_tick;
    }

    // Check if we hit the limit before reaching LCA
    if (pindex && pindex != lca) {
        result.txids_capped = true;
    }

    // Data is complete if we have tick data and timestamps.
    // Txid data may be partial (pruned or capped) but that's acceptable.
    result.data_complete = have_lca && have_tip && (lca_first_seen > 0) && (tip_first_seen > 0);
    return result;
}

double ComputeTxOverlap(const SegmentStats& seg_a, const SegmentStats& seg_b)
{
    if (seg_a.txids.empty() && seg_b.txids.empty()) {
        return 100.0;  // Both empty - degenerate case
    }

    // Compute intersection
    std::set<uint256> intersection;
    std::set_intersection(
        seg_a.txids.begin(), seg_a.txids.end(),
        seg_b.txids.begin(), seg_b.txids.end(),
        std::inserter(intersection, intersection.begin())
    );

    // Compute union size
    std::set<uint256> union_set;
    std::set_union(
        seg_a.txids.begin(), seg_a.txids.end(),
        seg_b.txids.begin(), seg_b.txids.end(),
        std::inserter(union_set, union_set.begin())
    );

    if (union_set.empty()) {
        return 100.0;
    }

    // Jaccard index
    return 100.0 * static_cast<double>(intersection.size()) / static_cast<double>(union_set.size());
}

int64_t ComputeFirstBlockDelay(
    const BlockDataSource& source,
    const CBlockIndex* lca,
    const CBlockIndex* first_fork_block,
    double sec_per_tick)
{
    if (!first_fork_block) {
        return 0;
    }

    int64_t first_seen = source.GetFirstSeenTs(first_fork_block->GetBlockHash());
    if (first_seen <= 0) {
        return 0;  // Unknown first_seen
    }

    // Delay vs miner timestamp (always available from CBlockIndex, no disk read)
    int64_t delta1 = std::max<int64_t>(0, first_seen - static_cast<int64_t>(first_fork_block->nTime));

    // Delay vs VDF-based expected time (requires disk reads, may fail on pruned nodes)
    int64_t delta2 = 0;
    if (lca && sec_per_tick > 0.0) {
        CBlock lca_blk, fork_blk;
        bool have_lca = source.ReadBlock(*lca, lca_blk);
        bool have_fork = source.ReadBlock(*first_fork_block, fork_blk);

        if (have_lca && have_fork) {
            uint64_t ticks_since_lca = fork_blk.cumulative_tick - lca_blk.cumulative_tick;
            double expected_time_from_ticks = static_cast<double>(lca->nTime) +
                                              static_cast<double>(ticks_since_lca) * sec_per_tick;
            delta2 = std::max<int64_t>(0, first_seen - static_cast<int64_t>(expected_time_from_ticks));
        }
        // If blocks are pruned, we fall back to delta1 (miner timestamp-based delay)
    }

    return std::max(delta1, delta2);
}

ReorgAdvisory GenerateReorgAdvisory(
    ChainstateManager& chainman,
    const node::BlockManager& blockman,
    const CBlockIndex* current_tip,
    const CBlockIndex* fork_tip,
    const CBlockIndex* lca)
{
    CachedBlockDataSource source(chainman, blockman);
    return GenerateReorgAdvisory(source, current_tip, fork_tip, lca);
}

ReorgAdvisory GenerateReorgAdvisory(
    const BlockDataSource& source,
    const CBlockIndex* current_tip,
    const CBlockIndex* fork_tip,
    const CBlockIndex* lca,
    int calibration_window,
    int calibration_min_window)
{
    ReorgAdvisory result;

    if (!current_tip || !fork_tip || !lca) {
        return result;
    }

    result.lca_height = lca->nHeight;
    result.depth_current = current_tip->nHeight - lca->nHeight;
    result.depth_fork = fork_tip->nHeight - lca->nHeight;

    // Calibrate tick-to-time
    result.calibration = CalibrateTickToTime(source, lca, calibration_window, calibration_min_window);

    double sec_per_tick = result.calibration.is_valid ? result.calibration.sec_per_tick : 0.0;

    // Compute segment stats
    result.seg_current = ComputeSegmentStats(source, lca, current_tip, sec_per_tick);
    result.seg_fork = ComputeSegmentStats(source, lca, fork_tip, sec_per_tick);

    // Transaction overlap
    result.tx_overlap_pct = ComputeTxOverlap(result.seg_current, result.seg_fork);

    // First block delay - find first block on fork after LCA
    const CBlockIndex* first_fork_block = fork_tip;
    while (first_fork_block && first_fork_block->pprev != lca) {
        first_fork_block = first_fork_block->pprev;
    }
    result.first_block_delay_secs = ComputeFirstBlockDelay(
        source, lca, first_fork_block, sec_per_tick);

    // Hashrate percentages (relative to baseline)
    // Use full arith_uint256 conversion to double for better accuracy.
    if (result.calibration.is_valid && result.calibration.baseline_hashrate > 0.0) {
        // Current segment hashrate using min(clock_time, tick_time) for upper bound
        double time_current = 0.0;
        if (result.seg_current.clock_time_secs > 0) {
            double tick_time = result.calibration.sec_per_tick * static_cast<double>(result.seg_current.ticks_diff);
            time_current = (tick_time > 0) ?
                std::min(static_cast<double>(result.seg_current.clock_time_secs), tick_time) :
                static_cast<double>(result.seg_current.clock_time_secs);
        } else if (result.seg_current.miner_time_secs > 0) {
            // Fallback to miner timestamps if first_seen unavailable
            time_current = static_cast<double>(result.seg_current.miner_time_secs);
        }

        if (time_current > 0.0 && result.seg_current.work_diff != arith_uint256(0)) {
            double work_approx = result.seg_current.work_diff.getdouble();
            double hashrate_current = work_approx / time_current;
            result.hashrate_current_pct = 100.0 * hashrate_current / result.calibration.baseline_hashrate;
        }

        // Fork segment hashrate (use clock time, fallback to miner time)
        double time_fork = 0.0;
        if (result.seg_fork.clock_time_secs > 0) {
            time_fork = static_cast<double>(result.seg_fork.clock_time_secs);
        } else if (result.seg_fork.miner_time_secs > 0) {
            time_fork = static_cast<double>(result.seg_fork.miner_time_secs);
        }

        if (time_fork > 0.0 && result.seg_fork.work_diff != arith_uint256(0)) {
            double work_approx = result.seg_fork.work_diff.getdouble();
            double hashrate_fork = work_approx / time_fork;
            result.hashrate_fork_pct = 100.0 * hashrate_fork / result.calibration.baseline_hashrate;
        }
    }

    // Since last block - fallback to miner timestamp if first_seen unavailable
    int64_t current_tip_first_seen = source.GetFirstSeenTs(current_tip->GetBlockHash());
    if (current_tip_first_seen > 0) {
        result.since_last_block_secs = GetTime() - current_tip_first_seen;
    } else {
        // Fallback: use miner timestamp
        result.since_last_block_secs = GetTime() - static_cast<int64_t>(current_tip->nTime);
    }

    // Advisory is valid if we have calibration and at least partial segment data
    result.is_valid = result.calibration.is_valid &&
                      (result.seg_current.block_count > 0 || result.seg_fork.block_count > 0);
    return result;
}

void AsyncLogReorgAdvisory(
    ChainstateManager& chainman,
    const node::BlockManager& blockman,
    const CBlockIndex* current_tip,
    const CBlockIndex* fork_tip,
    const CBlockIndex* lca)
{
    // Compute advisory synchronously to safely use chainman/blockman references.
    // Only the logging/storing is done asynchronously to avoid blocking cs_main.
    //
    // This design ensures:
    // - No dangling references (computation happens while references are valid)
    // - cs_main is released before I/O-heavy logging
    // - Advisory store updates happen off the hot path
    // - Clean shutdown via worker pool instead of detached threads
    ReorgAdvisory advisory;
    try {
        advisory = GenerateReorgAdvisory(chainman, blockman, current_tip, fork_tip, lca);
    } catch (const std::exception& e) {
        LogPrintf("REORG ADVISORY: Failed to compute advisory: %s\n", e.what());
        return;
    } catch (...) {
        LogPrintf("REORG ADVISORY: Failed to compute advisory: unknown error\n");
        return;
    }

    // Submit to worker pool for async logging/storing.
    // Worker pool handles queue management and clean shutdown.
    GetAdvisoryWorkerPool().Submit(std::move(advisory));
}

ReorgAdvisoryConfig GetReorgAdvisoryConfig()
{
    ReorgAdvisoryConfig config;

    // Read enabled flag (default: true)
    config.enabled = gArgs.GetBoolArg("-reorgadvisory", true);

    // Read depth threshold (default: 3)
    config.depth_threshold = gArgs.GetIntArg("-reorgadvisorydepth", ADVISORY_DEPTH_THRESHOLD);

    // Read offline threshold in seconds (default: 6 hours = 21600 seconds)
    config.offline_threshold_secs = gArgs.GetIntArg("-reorgadvisoryoffline", ADVISORY_OFFLINE_THRESHOLD_SECS);

    return config;
}

int GetMaxBlocksForTxids()
{
    int val = gArgs.GetIntArg("-reorgadvisorytxidblocks", DEFAULT_MAX_BLOCKS_FOR_TXIDS);
    // Clamp to reasonable range: 0 means unlimited (not recommended), max 10000
    if (val < 0) val = 0;
    if (val > 10000) val = 10000;
    return val;
}

bool ShouldTriggerAdvisory(int depth_current, int64_t since_last_block,
                           const std::optional<ReorgAdvisoryConfig>& config_opt)
{
    // Get config from parameter or read from gArgs
    ReorgAdvisoryConfig config = config_opt.value_or(GetReorgAdvisoryConfig());

    // Check if advisory system is enabled
    if (!config.enabled) {
        return false;
    }

    // Skip advisory if we've been offline too long
    if (since_last_block > config.offline_threshold_secs) {
        return false;
    }

    // Only trigger for deep reorgs (> threshold blocks)
    return depth_current > config.depth_threshold;
}

// ============================================================================
// ReorgAdvisoryStore implementation
// ============================================================================

void ReorgAdvisoryStore::Add(const ReorgAdvisory& advisory)
{
    LOCK(m_mutex);
    m_advisories.push_front(advisory);  // Newest at front
    while (m_advisories.size() > MAX_STORED_ADVISORIES) {
        m_advisories.pop_back();  // Remove oldest
    }
}

std::optional<ReorgAdvisory> ReorgAdvisoryStore::GetLatest() const
{
    LOCK(m_mutex);
    if (m_advisories.empty()) {
        return std::nullopt;
    }
    return m_advisories.front();
}

std::vector<ReorgAdvisory> ReorgAdvisoryStore::GetRecent(size_t count) const
{
    LOCK(m_mutex);
    std::vector<ReorgAdvisory> result;
    result.reserve(std::min(count, m_advisories.size()));
    for (size_t i = 0; i < count && i < m_advisories.size(); ++i) {
        result.push_back(m_advisories[i]);
    }
    return result;
}

std::vector<ReorgAdvisory> ReorgAdvisoryStore::GetAll() const
{
    LOCK(m_mutex);
    return std::vector<ReorgAdvisory>(m_advisories.begin(), m_advisories.end());
}

size_t ReorgAdvisoryStore::Size() const
{
    LOCK(m_mutex);
    return m_advisories.size();
}

void ReorgAdvisoryStore::Clear()
{
    LOCK(m_mutex);
    m_advisories.clear();
}

ReorgAdvisoryStore& GetReorgAdvisoryStore()
{
    static ReorgAdvisoryStore store;
    return store;
}

// ============================================================================
// AdvisoryWorkerPool implementation
// ============================================================================

AdvisoryWorkerPool::~AdvisoryWorkerPool()
{
    Stop();
}

void AdvisoryWorkerPool::Start()
{
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) {
        return;  // Already started
    }

    m_shutdown.store(false);
    m_worker = std::thread([this]() { WorkerLoop(); });
}

void AdvisoryWorkerPool::Stop()
{
    if (!m_started.load()) {
        return;  // Never started
    }

    {
        LOCK(m_mutex);
        m_shutdown.store(true);
    }
    m_cv.notify_all();

    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_started.store(false);
}

void AdvisoryWorkerPool::Submit(ReorgAdvisory advisory)
{
    // Start worker on first submit if not already running
    if (!m_started.load()) {
        Start();
    }

    {
        LOCK(m_mutex);
        if (m_shutdown.load()) {
            return;  // Don't accept new work during shutdown
        }

        // If queue is full, drop oldest to make room
        while (m_queue.size() >= MAX_QUEUE_SIZE) {
            m_queue.pop_front();
            LogPrintf("REORG ADVISORY: Queue full, dropping oldest advisory\n");
        }
        m_queue.push_back(std::move(advisory));
    }
    m_cv.notify_one();
}

void AdvisoryWorkerPool::WorkerLoop()
{
    util::ThreadRename("advisory-worker");

    while (true) {
        ReorgAdvisory advisory;
        {
            WAIT_LOCK(m_mutex, lock);
            m_cv.wait(lock, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
                return m_shutdown.load() || !m_queue.empty();
            });

            if (m_shutdown.load() && m_queue.empty()) {
                break;  // Shutdown and queue drained
            }

            if (m_queue.empty()) {
                continue;
            }

            advisory = std::move(m_queue.front());
            m_queue.pop_front();
        }

        // Process the advisory (outside the lock)
        try {
            // Store in global advisory store for RPC access
            GetReorgAdvisoryStore().Add(advisory);

            // Log the advisory summary
            LogPrintf("REORG ADVISORY: %s\n", advisory.Summary());

            // Detailed log for debugging
            LogDebug(BCLog::VALIDATION,
                "ReorgAdvisory details: lca_height=%d, depth_cur=%d, depth_fork=%d, "
                "tx_overlap=%.2f%%, first_block_delay=%llds, "
                "seg_cur_blocks=%d, seg_fork_blocks=%d, "
                "seg_cur_txs=%zu, seg_fork_txs=%zu, "
                "calibration_valid=%s, sec_per_tick=%.6f\n",
                advisory.lca_height, advisory.depth_current, advisory.depth_fork,
                advisory.tx_overlap_pct, advisory.first_block_delay_secs,
                advisory.seg_current.block_count, advisory.seg_fork.block_count,
                advisory.seg_current.txids.size(), advisory.seg_fork.txids.size(),
                advisory.calibration.is_valid ? "true" : "false",
                advisory.calibration.sec_per_tick
            );

#if HAVE_SYSTEM
            // Execute operator notify command if configured
            std::string notify_cmd = gArgs.GetArg("-reorgadvisorynotify", "");
            if (!notify_cmd.empty()) {
                // Replace placeholders: %d=depth, %h=lca_height, %f=fork_depth, %o=overlap_pct
                ReplaceAll(notify_cmd, "%d", std::to_string(advisory.depth_current));
                ReplaceAll(notify_cmd, "%h", std::to_string(advisory.lca_height));
                ReplaceAll(notify_cmd, "%f", std::to_string(advisory.depth_fork));
                ReplaceAll(notify_cmd, "%o", std::to_string(static_cast<int>(advisory.tx_overlap_pct)));
                LogDebug(BCLog::VALIDATION, "Executing reorg advisory notify: %s\n", notify_cmd);
                runCommand(notify_cmd);
            }
#endif
        } catch (const std::exception& e) {
            LogPrintf("REORG ADVISORY: Error processing advisory: %s\n", e.what());
        } catch (...) {
            LogPrintf("REORG ADVISORY: Unknown error processing advisory\n");
        }
    }

    LogDebug(BCLog::VALIDATION, "Advisory worker pool stopped\n");
}

AdvisoryWorkerPool& GetAdvisoryWorkerPool()
{
    static AdvisoryWorkerPool pool;
    return pool;
}

void StopAdvisoryWorkerPool()
{
    GetAdvisoryWorkerPool().Stop();
}

// ============================================================================
// REORG GATING IMPLEMENTATION
// ============================================================================

ReorgGatingConfig GetReorgGatingConfig()
{
    ReorgGatingConfig config;
    // Reorg gating is an operator-safety feature for live networks: a deep reorg
    // blocks the node until a human answers the gate (or it times out). On regtest
    // there is no operator, so an automated deep reorg would stall for the full
    // decision timeout. Default gating off on regtest; an explicit
    // -reorgadvisorygating= still wins for tests that exercise gating directly.
    const bool gating_default{gArgs.GetChainType() == ChainType::REGTEST
                                  ? false
                                  : DEFAULT_REORG_GATING_ENABLED};
    config.enabled = gArgs.GetBoolArg("-reorgadvisorygating", gating_default);
    config.timeout_secs = gArgs.GetIntArg("-reorgadvisorytimeout", DEFAULT_REORG_DECISION_TIMEOUT_SECS);
    config.timeout_accept = gArgs.GetBoolArg("-reorgadvisorytimeoutaccept", DEFAULT_REORG_TIMEOUT_ACTION_ACCEPT);
    config.gating_depth_threshold = gArgs.GetIntArg("-reorgadvisorygatingdepth", ADVISORY_DEPTH_THRESHOLD);
    config.autofollow_sane_partitions = gArgs.GetBoolArg("-reorgadvisoryautofollow", DEFAULT_REORG_AUTOFOLLOW_SANE_PARTITIONS);
    config.autofollow_min_fork_hashrate_pct = gArgs.GetIntArg("-reorgadvisoryautofollowminforkhashrate", DEFAULT_REORG_AUTOFOLLOW_MIN_FORK_HASHRATE_PCT);
    config.autofollow_max_fork_hashrate_pct = gArgs.GetIntArg("-reorgadvisoryautofollowmaxforkhashrate", DEFAULT_REORG_AUTOFOLLOW_MAX_FORK_HASHRATE_PCT);
    config.autofollow_min_fork_to_current_ratio_pct = gArgs.GetIntArg("-reorgadvisoryautofollowminratio", DEFAULT_REORG_AUTOFOLLOW_MIN_FORK_TO_CURRENT_RATIO_PCT);
    config.autofollow_min_first_block_delay_secs = gArgs.GetIntArg("-reorgadvisoryautofollowmindelay", DEFAULT_REORG_AUTOFOLLOW_MIN_FIRST_BLOCK_DELAY_SECS);
    config.approval_ttl_secs = gArgs.GetIntArg("-reorgadvisoryapprovalttl", DEFAULT_REORG_APPROVAL_TTL_SECS);

    // Clamp timeout to reasonable range (1 minute to 24 hours)
    if (config.timeout_secs < 60) config.timeout_secs = 60;
    if (config.timeout_secs > 24 * 60 * 60) config.timeout_secs = 24 * 60 * 60;
    if (config.approval_ttl_secs < 60) config.approval_ttl_secs = 60;
    if (config.approval_ttl_secs > 24 * 60 * 60) config.approval_ttl_secs = 24 * 60 * 60;
    if (config.autofollow_min_fork_hashrate_pct < 1) config.autofollow_min_fork_hashrate_pct = 1;
    if (config.autofollow_max_fork_hashrate_pct < config.autofollow_min_fork_hashrate_pct) {
        config.autofollow_max_fork_hashrate_pct = config.autofollow_min_fork_hashrate_pct;
    }
    if (config.autofollow_min_fork_to_current_ratio_pct < 1) config.autofollow_min_fork_to_current_ratio_pct = 1;
    if (config.autofollow_min_first_block_delay_secs < 0) config.autofollow_min_first_block_delay_secs = 0;

    return config;
}

ReorgGatingManager::ReorgGatingManager()
{
    ReloadConfig();
}

void ReorgGatingManager::ReloadConfig()
{
    m_config = GetReorgGatingConfig();
}

bool ReorgGatingManager::IsEnabled() const
{
    return m_config.enabled;
}

void ReorgGatingManager::SetPending(const ReorgAdvisory& advisory,
                                     const uint256& candidate_tip_hash,
                                     const uint256& current_tip_hash,
                                     const uint256& fork_point_hash)
{
    LOCK(m_mutex);

    m_pending.is_pending = true;
    m_pending.advisory = advisory;
    m_pending.candidate_tip_hash = candidate_tip_hash;
    m_pending.current_tip_hash = current_tip_hash;
    m_pending.fork_point_hash = fork_point_hash;
    m_pending.pending_since = GetTime();
    m_pending.decision = ReorgDecision::NONE;

    LogPrintf("REORG GATING: Deep reorg detected (depth=%d). Awaiting operator decision.\n",
              advisory.depth_current);
    LogPrintf("REORG GATING: Current tip: %s, Candidate tip: %s\n",
              current_tip_hash.ToString(), candidate_tip_hash.ToString());
    LogPrintf("REORG GATING: Timeout in %d seconds. Use RPC 'submitreorgdecision' to accept or reject.\n",
              m_config.timeout_secs);
}

PendingReorgState ReorgGatingManager::GetPending() const
{
    LOCK(m_mutex);
    return m_pending;
}

bool ReorgGatingManager::HasPending() const
{
    LOCK(m_mutex);
    return m_pending.is_pending;
}

bool ReorgGatingManager::SubmitDecision(ReorgDecision decision)
{
    {
        LOCK(m_mutex);
        if (!m_pending.is_pending) {
            LogPrintf("REORG GATING: No pending reorg decision to submit.\n");
            return false;
        }

        if (decision != ReorgDecision::ACCEPT && decision != ReorgDecision::REJECT) {
            LogPrintf("REORG GATING: Invalid decision. Must be ACCEPT or REJECT.\n");
            return false;
        }

        m_pending.decision = decision;
        LogPrintf("REORG GATING: Operator decision received: %s\n",
                  decision == ReorgDecision::ACCEPT ? "ACCEPT" : "REJECT");
    }

    // Wake up any waiting thread
    m_cv.notify_all();
    return true;
}

ReorgDecision ReorgGatingManager::WaitForDecision()
{
    WAIT_LOCK(m_mutex, lock);

    if (!m_pending.is_pending) {
        return ReorgDecision::NONE;
    }

    // Calculate absolute deadline
    auto deadline = std::chrono::system_clock::now() +
                    std::chrono::seconds(m_config.timeout_secs);

    // Wait until decision is made or timeout
    while (m_pending.decision == ReorgDecision::NONE) {
        auto status = m_cv.wait_until(lock, deadline);

        if (status == std::cv_status::timeout) {
            // Timeout reached - use default action
            m_pending.decision = ReorgDecision::TIMEOUT;
            LogPrintf("REORG GATING: Decision timeout. Default action: %s\n",
                      m_config.timeout_accept ? "ACCEPT" : "REJECT");
            break;
        }

        // Check if decision was made while waiting
        if (m_pending.decision != ReorgDecision::NONE) {
            break;
        }
    }

    return m_pending.decision;
}

void ReorgGatingManager::ClearPending()
{
    LOCK(m_mutex);
    m_pending = PendingReorgState{};
    LogDebug(BCLog::VALIDATION, "REORG GATING: Pending state cleared.\n");
}

void ReorgGatingManager::RecordApprovalFromPending()
{
    LOCK(m_mutex);
    if (!m_pending.is_pending) {
        return;
    }

    m_approval.is_valid = true;
    m_approval.fork_point_hash = m_pending.fork_point_hash;
    m_approval.candidate_tip_hash = m_pending.candidate_tip_hash;
    m_approval.current_tip_hash = m_pending.current_tip_hash;
    m_approval.approved_at = GetTime();

    LogPrintf("REORG GATING: Recorded approval for reorg at fork point %s (candidate %s, valid for %d seconds). "
              "Later segments of this reorg will not re-prompt.\n",
              m_approval.fork_point_hash.ToString(),
              m_approval.candidate_tip_hash.ToString(),
              m_config.approval_ttl_secs);
}

ReorgApprovalState ReorgGatingManager::GetApproval() const
{
    LOCK(m_mutex);
    if (!m_approval.is_valid) {
        return ReorgApprovalState{};
    }
    if (GetTime() - m_approval.approved_at > m_config.approval_ttl_secs) {
        return ReorgApprovalState{};
    }
    return m_approval;
}

void ReorgGatingManager::ClearApproval()
{
    LOCK(m_mutex);
    m_approval = ReorgApprovalState{};
    LogDebug(BCLog::VALIDATION, "REORG GATING: Approval state cleared.\n");
}

int64_t ReorgGatingManager::GetTimeoutRemaining() const
{
    LOCK(m_mutex);
    if (!m_pending.is_pending) {
        return 0;
    }

    int64_t elapsed = GetTime() - m_pending.pending_since;
    int64_t remaining = m_config.timeout_secs - elapsed;
    return remaining > 0 ? remaining : 0;
}

ReorgGatingManager& GetReorgGatingManager()
{
    static ReorgGatingManager manager;
    return manager;
}

namespace {
//! Depth of nested ReorgGateOperatorAction scopes on this thread.
thread_local int g_reorg_gate_operator_action_depth{0};
} // namespace

ReorgGateOperatorAction::ReorgGateOperatorAction()
{
    ++g_reorg_gate_operator_action_depth;
}

ReorgGateOperatorAction::~ReorgGateOperatorAction()
{
    --g_reorg_gate_operator_action_depth;
}

bool ReorgGateOperatorActionActive()
{
    return g_reorg_gate_operator_action_depth > 0;
}

bool ShouldGateReorg(int depth_current, int64_t since_last_block, bool disconnect_only,
                     const std::optional<ReorgGatingConfig>& config_opt)
{
    // The gate exists to obtain operator sign-off. An operator-initiated
    // action (invalidateblock/reconsiderblock) already carries that sign-off,
    // so gating it would only block the RPC until the decision timeout.
    if (ReorgGateOperatorActionActive()) {
        LogPrintf("REORG GATING: Skipping gate - chain switch is operator-initiated.\n");
        return false;
    }

    // Local policy corrections can make the best chain an ancestor of the
    // current tip. That disconnect-only retreat is self-correction, not a
    // competing fork that needs operator approval.
    if (disconnect_only) {
        return false;
    }

    ReorgGatingConfig config = config_opt ? *config_opt : GetReorgGatingConfig();

    if (!config.enabled) {
        return false;
    }

    // Don't gate if below depth threshold
    if (depth_current <= config.gating_depth_threshold) {
        return false;
    }

    // Don't gate if node was offline too long (auto-follow per spec)
    ReorgAdvisoryConfig adv_config = GetReorgAdvisoryConfig();
    if (since_last_block > adv_config.offline_threshold_secs) {
        LogDebug(BCLog::VALIDATION,
            "REORG GATING: Skipping gate - node was offline for %lld seconds (threshold: %lld)\n",
            since_last_block, adv_config.offline_threshold_secs);
        return false;
    }

    return true;
}

bool ShouldAutoFollowSanePartitionReorg(const ReorgAdvisory& advisory,
                                        const std::optional<ReorgGatingConfig>& config_opt)
{
    const ReorgGatingConfig config = config_opt ? *config_opt : GetReorgGatingConfig();

    if (!config.autofollow_sane_partitions) {
        return false;
    }

    if (!advisory.is_valid || !advisory.calibration.is_valid) {
        return false;
    }

    if (advisory.depth_current <= config.gating_depth_threshold) {
        return false;
    }

    if (advisory.depth_fork <= 0 || advisory.seg_current.block_count <= 0 || advisory.seg_fork.block_count <= 0) {
        return false;
    }

    if (advisory.seg_fork.work_diff <= advisory.seg_current.work_diff) {
        return false;
    }

    if (advisory.first_block_delay_secs < config.autofollow_min_first_block_delay_secs) {
        return false;
    }

    const double fork_hashrate = advisory.hashrate_fork_pct;
    const double current_hashrate = advisory.hashrate_current_pct;

    if (!std::isfinite(fork_hashrate) || !std::isfinite(current_hashrate)) {
        return false;
    }

    if (fork_hashrate <= 0.0 || current_hashrate <= 0.0) {
        return false;
    }

    if (fork_hashrate < static_cast<double>(config.autofollow_min_fork_hashrate_pct) ||
        fork_hashrate > static_cast<double>(config.autofollow_max_fork_hashrate_pct)) {
        return false;
    }

    if ((fork_hashrate * 100.0) <
        (current_hashrate * static_cast<double>(config.autofollow_min_fork_to_current_ratio_pct))) {
        return false;
    }

    return true;
}
