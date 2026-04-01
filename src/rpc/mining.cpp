// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/system.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <deploymentinfo.h>
#include <deploymentstatus.h>
#include <interfaces/mining.h>
#include <key_io.h>
#include <net.h>
#include <node/context.h>
#include <node/extapi.h>
#include <node/miner.h>
#include <node/warnings.h>
#include <policy/ephemeral_policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <rpc/blockchain.h>
#include <rpc/blockheader_generated.h>
#include <rpc/mining.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <txmempool.h>
#include <univalue.h>
#include <util/signalinterrupt.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <validationapi.h>
#include <vdf/VdfVerify.h>
#include <verification/quick_verifier.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <span>
#include <stdint.h>
#include <vector>

using interfaces::BlockRef;
using interfaces::BlockTemplate;
using interfaces::Mining;
using node::BlockAssembler;
using node::GetMinimumTime;
using node::NodeContext;
using node::RegenerateCommitments;
using node::UpdateTime;
using util::ToString;

/**
 * Return average network hashes per second based on the last 'lookup' blocks,
 * or from the last difficulty change if 'lookup' is -1.
 * If 'height' is -1, compute the estimate from current chain tip.
 * If 'height' is a valid block height, compute the estimate at the time when a given block was found.
 */
static UniValue GetNetworkHashPS(int lookup, int height, const CChain& active_chain) {
    if (lookup < -1 || lookup == 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid nblocks. Must be a positive number or -1.");
    }

    if (height < -1 || height > active_chain.Height()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block does not exist at specified height");
    }

    const CBlockIndex* pb = active_chain.Tip();

    if (height >= 0) {
        pb = active_chain[height];
    }

    if (pb == nullptr || !pb->nHeight)
        return 0;

    // If lookup is -1, then use blocks since last difficulty change.
    if (lookup == -1)
        lookup = pb->nHeight % Params().GetConsensus().DifficultyAdjustmentInterval() + 1;

    // If lookup is larger than chain, then set it to chain length.
    if (lookup > pb->nHeight)
        lookup = pb->nHeight;

    const CBlockIndex* pb0 = pb;
    int64_t minTime = pb0->GetBlockTime();
    int64_t maxTime = minTime;
    for (int i = 0; i < lookup; i++) {
        pb0 = pb0->pprev;
        int64_t time = pb0->GetBlockTime();
        minTime = std::min(time, minTime);
        maxTime = std::max(time, maxTime);
    }

    // In case there's a situation where minTime == maxTime, we don't want a divide by zero exception.
    if (minTime == maxTime)
        return 0;

    arith_uint256 workDiff = pb->nChainWork - pb0->nChainWork;
    int64_t timeDiff = maxTime - minTime;

    return workDiff.getdouble() / timeDiff;
}

static RPCHelpMan getnetworkhashps()
{
    return RPCHelpMan{"getnetworkhashps",
                "\nReturns the estimated network hashes per second based on the last n blocks.\n"
                "Pass in [blocks] to override # of blocks, -1 specifies since last difficulty change.\n"
                "Pass in [height] to estimate the network speed at the time when a certain block was found.\n",
                {
                    {"nblocks", RPCArg::Type::NUM, RPCArg::Default{120}, "The number of previous blocks to calculate estimate from, or -1 for blocks since last difficulty change."},
                    {"height", RPCArg::Type::NUM, RPCArg::Default{-1}, "To estimate at the time of the given height."},
                },
                RPCResult{
                    RPCResult::Type::NUM, "", "Hashes per second estimated"},
                RPCExamples{
                    HelpExampleCli("getnetworkhashps", "")
            + HelpExampleRpc("getnetworkhashps", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    LOCK(cs_main);
    return GetNetworkHashPS(self.Arg<int>("nblocks"), self.Arg<int>("height"), chainman.ActiveChain());
},
    };
}

static bool GenerateBlock(NodeContext& node, ChainstateManager& chainman, CBlock&& block, uint64_t& max_tries, std::shared_ptr<const CBlock>& block_out, bool process_new_block)
{
    block_out.reset();
    block.hashMerkleRoot = BlockMerkleRoot(block);

    // Update hashPoW after recalculating merkle root (it may contain the merkle commitment)
    int nHeight{-1};
    {
        LOCK(cs_main);
        const CBlockIndex* pindexPrev = chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock);
        if (pindexPrev) {
            nHeight = pindexPrev->nHeight + 1;
        }
    }
    if (nHeight != -1) {
        const bool use_merkle = chainman.GetConsensus().IsVdfSpvActive(nHeight);
        block.hashPoW = block.pow.GetCommitment(use_merkle);
    }

    // On mockable/test chains (e.g., regtest, testnet4, tensor-reg), bypass the external
    // mining API and use the local nonce scan against nAdjBits + short hash.
    // This keeps functional tests fast and self-contained.
    if (!chainman.GetConsensus().external_api || chainman.GetParams().IsMockableChain()) {
        // Ensure adjusted bits are initialized for Tensor header
        if (block.nAdjBits == 0) block.nAdjBits = block.nBits;

        // Scan nonces until PoW target is met or we run out of tries
        (void)block.GetShortHash();
        while (max_tries > 0 && block.nNonce < std::numeric_limits<uint32_t>::max() &&
               !CheckProofOfWork(block.GetShortHash(), block.nAdjBits, chainman.GetConsensus()) &&
               !chainman.m_interrupt) {
            ++block.nNonce;
            --max_tries;
        }
        if (max_tries == 0 || chainman.m_interrupt) {
            return false;
        }
        if (block.nNonce == std::numeric_limits<uint32_t>::max()) {
            return true;
        }
    } else {
        // Production-like chains may require the external PoW provider
        if (node.expt_api) {
            node.expt_api->SendApiRequest(block);
            if (!node.expt_api->GetApiAnswer(block, true)) {
                return false;
            }
        } else {
            // No external API available; fail fast instead of hanging
            throw JSONRPCError(RPC_INTERNAL_ERROR, "External mining API not available");
        }
    }

    block_out = std::make_shared<const CBlock>(std::move(block));

    if (!process_new_block) return true;

    if (!chainman.ProcessNewBlock(block_out, /*force_processing=*/true, /*min_pow_checked=*/true, nullptr)) {
        // On external-validation chains, a false here can mean "queued for async validation".
        if (chainman.GetConsensus().external_api && g_ValidationApi &&
            !g_ValidationApi->UsesRequestStatusForBlockProcessing()) {
            // If Quick already failed, propagate the error immediately.
            ValidationResponseValue validation_status;
            if (g_ValidationApi->GetRequestStatus(block_out->GetHash(), ValidationReqType::Quick, validation_status, false)) {
                if (validation_status == ValidationResponseValue::Quick_Fail) {
                    throw JSONRPCError(RPC_INTERNAL_ERROR, "ProcessNewBlock, quick validation failed");
                }
            }

            // The validation worker will retry ProcessNewBlock when Full validation resolves.
            // Do not block this RPC while the block is still Amber/pending.
            validation_status = static_cast<ValidationResponseValue>(g_ValidationApi->GetOwnFullStatus(block_out->GetHash()));
            const bool have_full_status{validation_status != ValidationResponseValue::Not_Checked};
            if (have_full_status) {
                if (validation_status != ValidationResponseValue::Full_Green &&
                    validation_status != ValidationResponseValue::Full_Red &&
                    validation_status != ValidationResponseValue::Full_Amber) {
                    throw JSONRPCError(RPC_INTERNAL_ERROR, "ProcessNewBlock, block not accepted");
                }
            }
            return true;
        }
        throw JSONRPCError(RPC_INTERNAL_ERROR, "ProcessNewBlock, block not accepted");
    }

    return true;
}

static UniValue generateBlocks(NodeContext& node, ChainstateManager& chainman, Mining& miner, const CScript& coinbase_output_script, int nGenerate, uint64_t nMaxTries)
{
    UniValue blockHashes(UniValue::VARR);
    while (nGenerate > 0 && !chainman.m_interrupt) {
        std::unique_ptr<BlockTemplate> block_template(miner.createNewBlock({ .coinbase_output_script = coinbase_output_script }));
        CHECK_NONFATAL(block_template);

        std::shared_ptr<const CBlock> block_out;
        if (!GenerateBlock(node, chainman, block_template->getBlock(), nMaxTries, block_out, /*process_new_block=*/true)) {
            break;
        }

        if (block_out) {
            --nGenerate;
            blockHashes.push_back(block_out->GetHash().GetHex());
        }
    }
    return blockHashes;
}

static bool getScriptFromDescriptor(const std::string& descriptor, CScript& script, std::string& error)
{
    FlatSigningProvider key_provider;
    const auto descs = Parse(descriptor, key_provider, error, /* require_checksum = */ false);
    if (descs.empty()) return false;
    if (descs.size() > 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Multipath descriptor not accepted");
    }
    const auto& desc = descs.at(0);
    if (desc->IsRange()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Ranged descriptor not accepted. Maybe pass through deriveaddresses first?");
    }

    FlatSigningProvider provider;
    std::vector<CScript> scripts;
    if (!desc->Expand(0, key_provider, scripts, provider)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Cannot derive script without private keys");
    }

    // Combo descriptors can have 2 or 4 scripts, so we can't just check scripts.size() == 1
    CHECK_NONFATAL(scripts.size() > 0 && scripts.size() <= 4);

    if (scripts.size() == 1) {
        script = scripts.at(0);
    } else if (scripts.size() == 4) {
        // For uncompressed keys, take the 3rd script, since it is p2wpkh
        script = scripts.at(2);
    } else {
        // Else take the 2nd script, since it is p2pkh
        script = scripts.at(1);
    }

    return true;
}

static RPCHelpMan generatetodescriptor()
{
    return RPCHelpMan{
        "generatetodescriptor",
        "Mine to a specified descriptor and return the block hashes.",
        {
            {"num_blocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "How many blocks are generated."},
            {"descriptor", RPCArg::Type::STR, RPCArg::Optional::NO, "The descriptor to send the newly generated bitcoin to."},
            {"maxtries", RPCArg::Type::NUM, RPCArg::Default{DEFAULT_MAX_TRIES}, "How many iterations to try."},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "hashes of blocks generated",
            {
                {RPCResult::Type::STR_HEX, "", "blockhash"},
            }
        },
        RPCExamples{
            "\nGenerate 11 blocks to mydesc\n" + HelpExampleCli("generatetodescriptor", "11 \"mydesc\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const auto num_blocks{self.Arg<int>("num_blocks")};
    const auto max_tries{self.Arg<uint64_t>("maxtries")};

    CScript coinbase_output_script;
    std::string error;
    if (!getScriptFromDescriptor(self.Arg<std::string>("descriptor"), coinbase_output_script, error)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error);
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    Mining& miner = EnsureMining(node);
    ChainstateManager& chainman = EnsureChainman(node);

    return generateBlocks(node, chainman, miner, coinbase_output_script, num_blocks, max_tries);
},
    };
}

static RPCHelpMan generate()
{
    return RPCHelpMan{"generate", "has been replaced by the -generate cli option. Refer to -help for more information.", {}, {}, RPCExamples{""}, [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, self.ToString());
    }};
}

static RPCHelpMan generatetoaddress()
{
    return RPCHelpMan{"generatetoaddress",
        "Mine to a specified address and return the block hashes.",
         {
             {"nblocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "How many blocks are generated."},
             {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address to send the newly generated bitcoin to."},
             {"maxtries", RPCArg::Type::NUM, RPCArg::Default{DEFAULT_MAX_TRIES}, "How many iterations to try."},
         },
         RPCResult{
             RPCResult::Type::ARR, "", "hashes of blocks generated",
             {
                 {RPCResult::Type::STR_HEX, "", "blockhash"},
             }},
         RPCExamples{
            "\nGenerate 11 blocks to myaddress\n"
            + HelpExampleCli("generatetoaddress", "11 \"myaddress\"")
            + "If you are using the " CLIENT_NAME " wallet, you can get a new address to send the newly generated bitcoin to with:\n"
            + HelpExampleCli("getnewaddress", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const int num_blocks{request.params[0].getInt<int>()};
    const uint64_t max_tries{request.params[2].isNull() ? DEFAULT_MAX_TRIES : request.params[2].getInt<int>()};

    CTxDestination destination = DecodeDestination(request.params[1].get_str());
    if (!IsValidDestination(destination)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address");
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    Mining& miner = EnsureMining(node);
    ChainstateManager& chainman = EnsureChainman(node);

    CScript coinbase_output_script = GetScriptForDestination(destination);

    return generateBlocks(node, chainman, miner, coinbase_output_script, num_blocks, max_tries);
},
    };
}

static RPCHelpMan generateblock()
{
    return RPCHelpMan{"generateblock",
        "Mine a set of ordered transactions to a specified address or descriptor and return the block hash.",
        {
            {"output", RPCArg::Type::STR, RPCArg::Optional::NO, "The address or descriptor to send the newly generated bitcoin to."},
            {"transactions", RPCArg::Type::ARR, RPCArg::Optional::NO, "An array of hex strings which are either txids or raw transactions.\n"
                "Txids must reference transactions currently in the mempool.\n"
                "All transactions must be valid and in valid order, otherwise the block will be rejected.",
                {
                    {"rawtx/txid", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, ""},
                },
            },
            {"submit", RPCArg::Type::BOOL, RPCArg::Default{true}, "Whether to submit the block before the RPC call returns or to return it as hex."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "hash", "hash of generated block"},
                {RPCResult::Type::STR_HEX, "hex", /*optional=*/true, "hex of generated block, only present when submit=false"},
            }
        },
        RPCExamples{
            "\nGenerate a block to myaddress, with txs rawtx and mempool_txid\n"
            + HelpExampleCli("generateblock", R"("myaddress" '["rawtx", "mempool_txid"]')")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const auto address_or_descriptor = request.params[0].get_str();
    CScript coinbase_output_script;
    std::string error;

    if (!getScriptFromDescriptor(address_or_descriptor, coinbase_output_script, error)) {
        const auto destination = DecodeDestination(address_or_descriptor);
        if (!IsValidDestination(destination)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address or descriptor");
        }

        coinbase_output_script = GetScriptForDestination(destination);
    }

    NodeContext& node = EnsureAnyNodeContext(request.context);
    Mining& miner = EnsureMining(node);
    const CTxMemPool& mempool = EnsureMemPool(node);

    std::vector<CTransactionRef> txs;
    const auto raw_txs_or_txids = request.params[1].get_array();
    for (size_t i = 0; i < raw_txs_or_txids.size(); i++) {
        const auto& str{raw_txs_or_txids[i].get_str()};

        CMutableTransaction mtx;
        if (auto hash{uint256::FromHex(str)}) {
            const auto tx{mempool.get(*hash)};
            if (!tx) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Transaction %s not in mempool.", str));
            }

            txs.emplace_back(tx);

        } else if (DecodeHexTx(mtx, str)) {
            txs.push_back(MakeTransactionRef(std::move(mtx)));

        } else {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("Transaction decode failed for %s. Make sure the tx has at least one input.", str));
        }
    }

    const bool process_new_block{request.params[2].isNull() ? true : request.params[2].get_bool()};
    CBlock block;

    ChainstateManager& chainman = EnsureChainman(node);
    {
        LOCK(chainman.GetMutex());
        {
            std::unique_ptr<BlockTemplate> block_template{miner.createNewBlock({.use_mempool = false, .coinbase_output_script = coinbase_output_script})};
            CHECK_NONFATAL(block_template);

            block = block_template->getBlock();
        }

        CHECK_NONFATAL(block.vtx.size() == 1);

        // Add transactions
        block.vtx.insert(block.vtx.end(), txs.begin(), txs.end());
        RegenerateCommitments(block, chainman);

        BlockValidationState state;
        if (!TestBlockValidity(state, chainman.GetParams(), chainman.ActiveChainstate(), block, chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock),  /*fCheckApi=*/false, /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false)) {
            throw JSONRPCError(RPC_VERIFY_ERROR, strprintf("TestBlockValidity failed: %s", state.ToString()));
        }
    }

    std::shared_ptr<const CBlock> block_out;
    uint64_t max_tries{DEFAULT_MAX_TRIES};

    if (!GenerateBlock(node, chainman, std::move(block), max_tries, block_out, process_new_block) || !block_out) {
        throw JSONRPCError(RPC_MISC_ERROR, "Failed to make block.");
    }

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("hash", block_out->GetHash().GetHex());
    if (!process_new_block) {
        DataStream block_ser;
        block_ser << TX_WITH_WITNESS(*block_out);
        obj.pushKV("hex", HexStr(block_ser));
    }
    return obj;
},
    };
}

static RPCHelpMan getmininginfo()
{
    return RPCHelpMan{"getmininginfo",
                "\nReturns a json object containing mining-related information.",
                {},
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "blocks", "The current block"},
                        {RPCResult::Type::NUM, "currentblockweight", /*optional=*/true, "The block weight (including reserved weight for block header, txs count and coinbase tx) of the last assembled block (only present if a block was ever assembled)"},
                        {RPCResult::Type::NUM, "currentblocktx", /*optional=*/true, "The number of block transactions (excluding coinbase) of the last assembled block (only present if a block was ever assembled)"},
                        {RPCResult::Type::STR_HEX, "bits", "The current nBits, compact representation of the block difficulty target"},
                        {RPCResult::Type::NUM, "difficulty", "The current difficulty"},
                        {RPCResult::Type::STR_HEX, "target", "The current target"},
                        {RPCResult::Type::NUM, "networkhashps", "The network hashes per second"},
                        {RPCResult::Type::NUM, "pooledtx", "The size of the mempool"},
                        {RPCResult::Type::STR, "chain", "current network name (" LIST_CHAIN_NAMES ")"},
                        {RPCResult::Type::STR_HEX, "signet_challenge", /*optional=*/true, "The block challenge (aka. block script), in hexadecimal (only present if the current network is a signet)"},
                        {RPCResult::Type::OBJ, "next", "The next block",
                        {
                            {RPCResult::Type::NUM, "height", "The next height"},
                            {RPCResult::Type::STR_HEX, "bits", "The next target nBits"},
                            {RPCResult::Type::NUM, "difficulty", "The next difficulty"},
                            {RPCResult::Type::STR_HEX, "target", "The next target"}
                        }},
                        {RPCResult::Type::STR_HEX, "tip_hash", "Hash of the current chain tip"},
                        {RPCResult::Type::NUM_TIME, "tip_time", "Unix timestamp of the current chain tip"},
                        {RPCResult::Type::NUM, "tip_age_seconds", "Seconds elapsed since the current chain tip's nTime (used by the broker to verify template freshness)"},
                        (IsDeprecatedRPCEnabled("warnings") ?
                            RPCResult{RPCResult::Type::STR, "warnings", "any network and blockchain warnings (DEPRECATED)"} :
                            RPCResult{RPCResult::Type::ARR, "warnings", "any network and blockchain warnings (run with `-deprecatedrpc=warnings` to return the latest warning as a single string)",
                            {
                                {RPCResult::Type::STR, "", "warning"},
                            }
                            }
                        ),
                    }},
                RPCExamples{
                    HelpExampleCli("getmininginfo", "")
            + HelpExampleRpc("getmininginfo", "")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    const CTxMemPool& mempool = EnsureMemPool(node);
    ChainstateManager& chainman = EnsureChainman(node);
    LOCK(cs_main);
    const CChain& active_chain = chainman.ActiveChain();
    CBlockIndex& tip{*CHECK_NONFATAL(active_chain.Tip())};

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("blocks",           active_chain.Height());
    if (BlockAssembler::m_last_block_weight) obj.pushKV("currentblockweight", *BlockAssembler::m_last_block_weight);
    if (BlockAssembler::m_last_block_num_txs) obj.pushKV("currentblocktx", *BlockAssembler::m_last_block_num_txs);
    obj.pushKV("bits", strprintf("%08x", tip.nBits));
    obj.pushKV("difficulty", GetDifficulty(tip));
    obj.pushKV("target", GetTarget(tip, chainman.GetConsensus().powLimit).GetHex());
    obj.pushKV("networkhashps",    getnetworkhashps().HandleRequest(request));
    obj.pushKV("pooledtx",         (uint64_t)mempool.size());
    obj.pushKV("chain", chainman.GetParams().GetChainTypeString());

    UniValue next(UniValue::VOBJ);
    CBlockIndex next_index;
    NextEmptyBlockIndex(tip, chainman.GetConsensus(), next_index);

    next.pushKV("height", next_index.nHeight);
    next.pushKV("bits", strprintf("%08x", next_index.nBits));
    next.pushKV("difficulty", GetDifficulty(next_index));
    next.pushKV("target", GetTarget(next_index, chainman.GetConsensus().powLimit).GetHex());
    obj.pushKV("next", next);

    if (chainman.GetParams().GetChainType() == ChainType::SIGNET) {
        const std::vector<uint8_t>& signet_challenge =
            chainman.GetConsensus().signet_challenge;
        obj.pushKV("signet_challenge", HexStr(signet_challenge));
    }

    // Tip freshness fields for broker-side template verification.
    obj.pushKV("tip_hash", tip.GetBlockHash().ToString());
    obj.pushKV("tip_time", static_cast<int64_t>(tip.nTime));
    obj.pushKV("tip_age_seconds", GetTime() - static_cast<int64_t>(tip.nTime));

    obj.pushKV("warnings", node::GetWarningsForRpc(*CHECK_NONFATAL(node.warnings), IsDeprecatedRPCEnabled("warnings")));
    return obj;
},
    };
}


// NOTE: Unlike wallet RPC (which use BTC values), mining RPCs follow GBT (BIP 22) in using satoshi amounts
static RPCHelpMan prioritisetransaction()
{
    return RPCHelpMan{"prioritisetransaction",
                "Accepts the transaction into mined blocks at a higher (or lower) priority\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id."},
                    {"dummy", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "API-Compatibility for previous API. Must be zero or null.\n"
            "                  DEPRECATED. For forward compatibility use named arguments and omit this parameter."},
                    {"fee_delta", RPCArg::Type::NUM, RPCArg::Optional::NO, "The fee value (in satoshis) to add (or subtract, if negative).\n"
            "                  Note, that this value is not a fee rate. It is a value to modify absolute fee of the TX.\n"
            "                  The fee is not actually paid, only the algorithm for selecting transactions into a block\n"
            "                  considers the transaction as it would have paid a higher (or lower) fee."},
                },
                RPCResult{
                    RPCResult::Type::BOOL, "", "Returns true"},
                RPCExamples{
                    HelpExampleCli("prioritisetransaction", "\"txid\" 0.0 10000")
            + HelpExampleRpc("prioritisetransaction", "\"txid\", 0.0, 10000")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    LOCK(cs_main);

    uint256 hash(ParseHashV(request.params[0], "txid"));
    const auto dummy{self.MaybeArg<double>("dummy")};
    CAmount nAmount = request.params[2].getInt<int64_t>();

    if (dummy && *dummy != 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Priority is no longer supported, dummy argument to prioritisetransaction must be 0.");
    }

    CTxMemPool& mempool = EnsureAnyMemPool(request.context);

    // Non-0 fee dust transactions are not allowed for entry, and modification not allowed afterwards
    const auto& tx = mempool.get(hash);
    if (mempool.m_opts.require_standard && tx && !GetDust(*tx, mempool.m_opts.dust_relay_feerate).empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Priority is not supported for transactions with dust outputs.");
    }

    mempool.PrioritiseTransaction(hash, nAmount);
    return true;
},
    };
}

static RPCHelpMan getprioritisedtransactions()
{
    return RPCHelpMan{"getprioritisedtransactions",
        "Returns a map of all user-created (see prioritisetransaction) fee deltas by txid, and whether the tx is present in mempool.",
        {},
        RPCResult{
            RPCResult::Type::OBJ_DYN, "", "prioritisation keyed by txid",
            {
                {RPCResult::Type::OBJ, "<transactionid>", "", {
                    {RPCResult::Type::NUM, "fee_delta", "transaction fee delta in satoshis"},
                    {RPCResult::Type::BOOL, "in_mempool", "whether this transaction is currently in mempool"},
                    {RPCResult::Type::NUM, "modified_fee", /*optional=*/true, "modified fee in satoshis. Only returned if in_mempool=true"},
                }}
            },
        },
        RPCExamples{
            HelpExampleCli("getprioritisedtransactions", "")
            + HelpExampleRpc("getprioritisedtransactions", "")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
        {
            NodeContext& node = EnsureAnyNodeContext(request.context);
            CTxMemPool& mempool = EnsureMemPool(node);
            UniValue rpc_result{UniValue::VOBJ};
            for (const auto& delta_info : mempool.GetPrioritisedTransactions()) {
                UniValue result_inner{UniValue::VOBJ};
                result_inner.pushKV("fee_delta", delta_info.delta);
                result_inner.pushKV("in_mempool", delta_info.in_mempool);
                if (delta_info.in_mempool) {
                    result_inner.pushKV("modified_fee", *delta_info.modified_fee);
                }
                rpc_result.pushKV(delta_info.txid.GetHex(), std::move(result_inner));
            }
            return rpc_result;
        },
    };
}


// NOTE: Assumes a conclusive result; if result is inconclusive, it must be handled by caller
static UniValue BIP22ValidationResult(const BlockValidationState& state)
{
    if (state.IsValid())
        return UniValue::VNULL;

    if (state.IsError())
        throw JSONRPCError(RPC_VERIFY_ERROR, state.ToString());
    if (state.IsInvalid())
    {
        std::string strRejectReason = state.GetRejectReason();
        if (strRejectReason.empty())
            return "rejected";
        return strRejectReason;
    }
    // Should be impossible
    return "valid?";
}

// Prefix rule name with ! if not optional, see BIP9
static std::string gbt_rule_value(const std::string& name, bool gbt_optional_rule)
{
    std::string s{name};
    if (!gbt_optional_rule) {
        s.insert(s.begin(), '!');
    }
    return s;
}

static RPCHelpMan getblocktemplate()
{
    return RPCHelpMan{"getblocktemplate",
        "\nIf the request parameters include a 'mode' key, that is used to explicitly select between the default 'template' request or a 'proposal'.\n"
        "It returns data needed to construct a block to work on.\n"
        "For full specification, see BIPs 22, 23, 9, and 145:\n"
        "    https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki\n"
        "    https://github.com/bitcoin/bips/blob/master/bip-0023.mediawiki\n"
        "    https://github.com/bitcoin/bips/blob/master/bip-0009.mediawiki#getblocktemplate_changes\n"
        "    https://github.com/bitcoin/bips/blob/master/bip-0145.mediawiki\n",
        {
            {"template_request", RPCArg::Type::OBJ, RPCArg::Optional::NO, "Format of the template",
            {
                {"mode", RPCArg::Type::STR, /* treat as named arg */ RPCArg::Optional::OMITTED, "This must be set to \"template\", \"proposal\" (see BIP 23), or omitted"},
                {"capabilities", RPCArg::Type::ARR, /* treat as named arg */ RPCArg::Optional::OMITTED, "A list of strings",
                {
                    {"str", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "client side supported feature, 'longpoll', 'coinbasevalue', 'proposal', 'serverlist', 'workid'"},
                }},
                {"rules", RPCArg::Type::ARR, RPCArg::Optional::NO, "A list of strings",
                {
                    {"segwit", RPCArg::Type::STR, RPCArg::Optional::NO, "(literal) indicates client side segwit support"},
                    {"str", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "other client side supported softfork deployment"},
                }},
                {"longpollid", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "delay processing request until the result would vary significantly from the \"longpollid\" of a prior template"},
                {"data", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "proposed block data to check, encoded in hexadecimal; valid only for mode=\"proposal\""},
            },
            },
        },
        {
            RPCResult{"If the proposal was accepted with mode=='proposal'", RPCResult::Type::NONE, "", ""},
            RPCResult{"If the proposal was not accepted with mode=='proposal'", RPCResult::Type::STR, "", "According to BIP22"},
            RPCResult{"Otherwise", RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "version", "The preferred block version"},
                {RPCResult::Type::ARR, "rules", "specific block rules that are to be enforced",
                {
                    {RPCResult::Type::STR, "", "name of a rule the client must understand to some extent; see BIP 9 for format"},
                }},
                {RPCResult::Type::OBJ_DYN, "vbavailable", "set of pending, supported versionbit (BIP 9) softfork deployments",
                {
                    {RPCResult::Type::NUM, "rulename", "identifies the bit number as indicating acceptance and readiness for the named softfork rule"},
                }},
                {RPCResult::Type::ARR, "capabilities", "",
                {
                    {RPCResult::Type::STR, "value", "A supported feature, for example 'proposal'"},
                }},
                {RPCResult::Type::NUM, "vbrequired", "bit mask of versionbits the server requires set in submissions"},
                {RPCResult::Type::STR, "previousblockhash", "The hash of current highest block"},
                {RPCResult::Type::ARR, "transactions", "contents of non-coinbase transactions that should be included in the next block",
                {
                    {RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::STR_HEX, "data", "transaction data encoded in hexadecimal (byte-for-byte)"},
                        {RPCResult::Type::STR_HEX, "txid", "transaction hash excluding witness data, shown in byte-reversed hex"},
                        {RPCResult::Type::STR_HEX, "hash", "transaction hash including witness data, shown in byte-reversed hex"},
                        {RPCResult::Type::ARR, "depends", "array of numbers",
                        {
                            {RPCResult::Type::NUM, "", "transactions before this one (by 1-based index in 'transactions' list) that must be present in the final block if this one is"},
                        }},
                        {RPCResult::Type::NUM, "fee", "difference in value between transaction inputs and outputs (in satoshis); for coinbase transactions, this is a negative Number of the total collected block fees (ie, not including the block subsidy); if key is not present, fee is unknown and clients MUST NOT assume there isn't one"},
                        {RPCResult::Type::NUM, "sigops", "total SigOps cost, as counted for purposes of block limits; if key is not present, sigop cost is unknown and clients MUST NOT assume it is zero"},
                        {RPCResult::Type::NUM, "weight", "total transaction weight, as counted for purposes of block limits"},
                    }},
                }},
                {RPCResult::Type::OBJ_DYN, "coinbaseaux", "data that should be included in the coinbase's scriptSig content",
                {
                    {RPCResult::Type::STR_HEX, "key", "values must be in the coinbase (keys may be ignored)"},
                }},
                {RPCResult::Type::NUM, "coinbasevalue", "maximum allowable input to coinbase transaction, including the generation award and transaction fees (in satoshis)"},
                {RPCResult::Type::STR, "longpollid", "an id to include with a request to longpoll on an update to this template"},
                {RPCResult::Type::STR, "target", "The hash target"},
                {RPCResult::Type::NUM_TIME, "mintime", "The minimum timestamp appropriate for the next block time, expressed in " + UNIX_EPOCH_TIME + ". Adjusted for the proposed BIP94 timewarp rule."},
                {RPCResult::Type::ARR, "mutable", "list of ways the block template may be changed",
                {
                    {RPCResult::Type::STR, "value", "A way the block template may be changed, e.g. 'time', 'transactions', 'prevblock'"},
                }},
                {RPCResult::Type::STR_HEX, "noncerange", "A range of valid nonces"},
                {RPCResult::Type::NUM, "sigoplimit", "limit of sigops in blocks"},
                {RPCResult::Type::NUM, "sizelimit", "limit of block size"},
                {RPCResult::Type::NUM, "weightlimit", /*optional=*/true, "limit of block weight"},
                {RPCResult::Type::NUM_TIME, "curtime", "current timestamp in " + UNIX_EPOCH_TIME + ". Adjusted for the proposed BIP94 timewarp rule."},
                {RPCResult::Type::STR, "bits", "compressed target of next block"},
                {RPCResult::Type::NUM, "height", "The height of the next block"},
                {RPCResult::Type::STR_HEX, "signet_challenge", /*optional=*/true, "Only on signet"},
                {RPCResult::Type::STR_HEX, "default_witness_commitment", /*optional=*/true, "a valid witness commitment for the unmodified block template"},
            }},
        },
        RPCExamples{
                    HelpExampleCli("getblocktemplate", "'{\"rules\": [\"segwit\"]}'")
            + HelpExampleRpc("getblocktemplate", "{\"rules\": [\"segwit\"]}")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    Mining& miner = EnsureMining(node);
    LOCK(cs_main);
    uint256 tip{CHECK_NONFATAL(miner.getTip()).value().hash};

    std::string strMode = "template";
    UniValue lpval = NullUniValue;
    std::set<std::string> setClientRules;
    if (!request.params[0].isNull())
    {
        const UniValue& oparam = request.params[0].get_obj();
        const UniValue& modeval = oparam.find_value("mode");
        if (modeval.isStr())
            strMode = modeval.get_str();
        else if (modeval.isNull())
        {
            /* Do nothing */
        }
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
        lpval = oparam.find_value("longpollid");

        if (strMode == "proposal")
        {
            const UniValue& dataval = oparam.find_value("data");
            if (!dataval.isStr())
                throw JSONRPCError(RPC_TYPE_ERROR, "Missing data String key for proposal");

            CBlock block;
            if (!DecodeHexBlk(block, dataval.get_str()))
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");

            uint256 hash = block.GetHash();
            const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(hash);
            if (pindex) {
                if (pindex->IsValid(BLOCK_VALID_SCRIPTS))
                    return "duplicate";
                if (pindex->nStatus & BLOCK_FAILED_MASK)
                    return "duplicate-invalid";
                return "duplicate-inconclusive";
            }

            // TestBlockValidity only supports blocks built on the current Tip
            if (block.hashPrevBlock != tip) {
                return "inconclusive-not-best-prevblk";
            }
            BlockValidationState state;
            TestBlockValidity(state, chainman.GetParams(), chainman.ActiveChainstate(), block, chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock), /*fCheckApi=*/false, /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/true);
            return BIP22ValidationResult(state);
        }

        const UniValue& aClientRules = oparam.find_value("rules");
        if (aClientRules.isArray()) {
            for (unsigned int i = 0; i < aClientRules.size(); ++i) {
                const UniValue& v = aClientRules[i];
                setClientRules.insert(v.get_str());
            }
        }
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    if (!miner.isTestChain()) {
        const CConnman& connman = EnsureConnman(node);
        if (connman.GetNodeCount(ConnectionDirection::Both) == 0) {
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, CLIENT_NAME " is not connected!");
        }

        if (miner.isInitialBlockDownload()) {
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, CLIENT_NAME " is in initial sync and waiting for blocks...");
        }
    }

    static unsigned int nTransactionsUpdatedLast;
    const CTxMemPool& mempool = EnsureMemPool(node);

    // Long Polling (BIP22)
    if (!lpval.isNull()) {
        /**
         * Wait to respond until either the best block changes, OR there are more
         * transactions.
         *
         * The check for new transactions first happens after 1 minute and
         * subsequently every 10 seconds. BIP22 does not require this particular interval.
         * On mainnet the mempool changes frequently enough that in practice this RPC
         * returns after 60 seconds, or sooner if the best block changes.
         *
         * getblocktemplate is unlikely to be called by bitcoin-cli, so
         * -rpcclienttimeout is not a concern. BIP22 recommends a long request timeout.
         *
         * The longpollid is assumed to be a tip hash if it has the right format.
         */
        uint256 hashWatchedChain;
        unsigned int nTransactionsUpdatedLastLP;

        if (lpval.isStr())
        {
            // Format: <hashBestChain><nTransactionsUpdatedLast>
            const std::string& lpstr = lpval.get_str();

            // Assume the longpollid is a block hash. If it's not then we return
            // early below.
            hashWatchedChain = ParseHashV(lpstr.substr(0, 64), "longpollid");
            nTransactionsUpdatedLastLP = LocaleIndependentAtoi<int64_t>(lpstr.substr(64));
        }
        else
        {
            // NOTE: Spec does not specify behaviour for non-string longpollid, but this makes testing easier
            hashWatchedChain = tip;
            nTransactionsUpdatedLastLP = nTransactionsUpdatedLast;
        }

        // Release lock while waiting
        LEAVE_CRITICAL_SECTION(cs_main);
        {
            MillisecondsDouble checktxtime{std::chrono::minutes(1)};
            while (IsRPCRunning()) {
                // If hashWatchedChain is not a real block hash, this will
                // return immediately.
                std::optional<BlockRef> maybe_tip{miner.waitTipChanged(hashWatchedChain, checktxtime)};
                // Node is shutting down
                if (!maybe_tip) break;
                tip = maybe_tip->hash;
                if (tip != hashWatchedChain) break;

                // Check transactions for update without holding the mempool
                // lock to avoid deadlocks.
                if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLastLP) {
                    break;
                }
                checktxtime = std::chrono::seconds(10);
            }
        }
        ENTER_CRITICAL_SECTION(cs_main);

        tip = CHECK_NONFATAL(miner.getTip()).value().hash;

        if (!IsRPCRunning())
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        // TODO: Maybe recheck connections/IBD and (if something wrong) send an expires-immediately template to stop miners?
    }

    const Consensus::Params& consensusParams = chainman.GetParams().GetConsensus();

    // GBT must be called with 'signet' set in the rules for signet chains
    if (consensusParams.signet_blocks && setClientRules.count("signet") != 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "getblocktemplate must be called with the signet rule set (call with {\"rules\": [\"segwit\", \"signet\"]})");
    }

    // GBT must be called with 'segwit' set in the rules
    if (setClientRules.count("segwit") != 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "getblocktemplate must be called with the segwit rule set (call with {\"rules\": [\"segwit\"]})");
    }

    // Update block
    static CBlockIndex* pindexPrev;
    static int64_t time_start;
    static std::unique_ptr<BlockTemplate> block_template;
    if (!pindexPrev || pindexPrev->GetBlockHash() != tip ||
        (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - time_start > 5))
    {
        // Clear pindexPrev so future calls make a new block, despite any failures from here on
        pindexPrev = nullptr;

        // Store the pindexBest used before createNewBlock, to avoid races
        nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
        CBlockIndex* pindexPrevNew = chainman.m_blockman.LookupBlockIndex(tip);
        time_start = GetTime();

        // Create new block
        block_template = miner.createNewBlock();
        CHECK_NONFATAL(block_template);


        // Need to update only after we know createNewBlock succeeded
        pindexPrev = pindexPrevNew;
    }
    CHECK_NONFATAL(pindexPrev);
    CBlock block{block_template->getBlock()};

    // Update nTime
    UpdateTime(&block, consensusParams, pindexPrev);
    block.nNonce = 0;

    // NOTE: If at some point we support pre-segwit miners post-segwit-activation, this needs to take segwit support into consideration
    const bool fPreSegWit = !DeploymentActiveAfter(pindexPrev, chainman, Consensus::DEPLOYMENT_SEGWIT);

    UniValue aCaps(UniValue::VARR); aCaps.push_back("proposal");

    UniValue transactions(UniValue::VARR);
    std::map<uint256, int64_t> setTxIndex;
    std::vector<CAmount> tx_fees{block_template->getTxFees()};
    std::vector<CAmount> tx_sigops{block_template->getTxSigops()};

    int i = 0;
    for (const auto& it : block.vtx) {
        const CTransaction& tx = *it;
        uint256 txHash = tx.GetHash();
        setTxIndex[txHash] = i++;

        if (tx.IsCoinBase())
            continue;

        UniValue entry(UniValue::VOBJ);

        entry.pushKV("data", EncodeHexTx(tx));
        entry.pushKV("txid", txHash.GetHex());
        entry.pushKV("hash", tx.GetWitnessHash().GetHex());

        UniValue deps(UniValue::VARR);
        for (const CTxIn &in : tx.vin)
        {
            if (setTxIndex.count(in.prevout.hash))
                deps.push_back(setTxIndex[in.prevout.hash]);
        }
        entry.pushKV("depends", std::move(deps));

        int index_in_template = i - 2;
        entry.pushKV("fee", tx_fees.at(index_in_template));
        int64_t nTxSigOps{tx_sigops.at(index_in_template)};
        if (fPreSegWit) {
            CHECK_NONFATAL(nTxSigOps % WITNESS_SCALE_FACTOR == 0);
            nTxSigOps /= WITNESS_SCALE_FACTOR;
        }
        entry.pushKV("sigops", nTxSigOps);
        entry.pushKV("weight", GetTransactionWeight(tx));

        transactions.push_back(std::move(entry));
    }

    UniValue aux(UniValue::VOBJ);

    arith_uint256 hashTarget = arith_uint256().SetCompact(block.nBits);

    UniValue aMutable(UniValue::VARR);
    aMutable.push_back("time");
    aMutable.push_back("transactions");
    aMutable.push_back("prevblock");

    UniValue result(UniValue::VOBJ);
    result.pushKV("capabilities", std::move(aCaps));

    UniValue aRules(UniValue::VARR);
    aRules.push_back("csv");
    if (!fPreSegWit) aRules.push_back("!segwit");
    if (consensusParams.signet_blocks) {
        // indicate to miner that they must understand signet rules
        // when attempting to mine with this template
        aRules.push_back("!signet");
    }

    UniValue vbavailable(UniValue::VOBJ);
    const auto gbtstatus = chainman.m_versionbitscache.GBTStatus(*pindexPrev, consensusParams);

    for (const auto& [name, info] : gbtstatus.signalling) {
        vbavailable.pushKV(gbt_rule_value(name, info.gbt_optional_rule), info.bit);
        if (!info.gbt_optional_rule && !setClientRules.count(name)) {
            // If the client doesn't support this, don't indicate it in the [default] version
            block.nVersion &= ~info.mask;
        }
    }

    for (const auto& [name, info] : gbtstatus.locked_in) {
        block.nVersion |= info.mask;
        vbavailable.pushKV(gbt_rule_value(name, info.gbt_optional_rule), info.bit);
        if (!info.gbt_optional_rule && !setClientRules.count(name)) {
            // If the client doesn't support this, don't indicate it in the [default] version
            block.nVersion &= ~info.mask;
        }
    }

    for (const auto& [name, info] : gbtstatus.active) {
        aRules.push_back(gbt_rule_value(name, info.gbt_optional_rule));
        if (!info.gbt_optional_rule && !setClientRules.count(name)) {
            // Not supported by the client; make sure it's safe to proceed
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Support for '%s' rule requires explicit client support", name));
        }
    }

    result.pushKV("version", block.nVersion);
    result.pushKV("rules", std::move(aRules));
    result.pushKV("vbavailable", std::move(vbavailable));
    result.pushKV("vbrequired", int(0));

    result.pushKV("previousblockhash", block.hashPrevBlock.GetHex());
    result.pushKV("transactions", std::move(transactions));
    result.pushKV("coinbaseaux", std::move(aux));
    result.pushKV("coinbasevalue", (int64_t)block.vtx[0]->vout[0].nValue);
    result.pushKV("longpollid", tip.GetHex() + ToString(nTransactionsUpdatedLast));
    result.pushKV("target", hashTarget.GetHex());
    result.pushKV("mintime", GetMinimumTime(pindexPrev, consensusParams.DifficultyAdjustmentInterval()));
    result.pushKV("mutable", std::move(aMutable));
    result.pushKV("noncerange", "00000000ffffffff");
    int64_t nSigOpLimit = MAX_BLOCK_SIGOPS_COST;
    int64_t nSizeLimit = MAX_BLOCK_SERIALIZED_SIZE;
    if (fPreSegWit) {
        CHECK_NONFATAL(nSigOpLimit % WITNESS_SCALE_FACTOR == 0);
        nSigOpLimit /= WITNESS_SCALE_FACTOR;
        CHECK_NONFATAL(nSizeLimit % WITNESS_SCALE_FACTOR == 0);
        nSizeLimit /= WITNESS_SCALE_FACTOR;
    }
    result.pushKV("sigoplimit", nSigOpLimit);
    result.pushKV("sizelimit", nSizeLimit);
    if (!fPreSegWit) {
        result.pushKV("weightlimit", (int64_t)MAX_BLOCK_WEIGHT);
    }
    result.pushKV("curtime", block.GetBlockTime());
    result.pushKV("bits", strprintf("%08x", block.nBits));
    result.pushKV("height", (int64_t)(pindexPrev->nHeight+1));

    if (consensusParams.signet_blocks) {
        result.pushKV("signet_challenge", HexStr(consensusParams.signet_challenge));
    }

    if (!block_template->getCoinbaseCommitment().empty()) {
        result.pushKV("default_witness_commitment", HexStr(block_template->getCoinbaseCommitment()));
    }

    return result;
},
    };
}

class submitblock_StateCatcher final : public CValidationInterface
{
public:
    uint256 hash;
    bool found{false};
    BlockValidationState state;

    explicit submitblock_StateCatcher(const uint256 &hashIn) : hash(hashIn), state() {}

protected:
    void BlockChecked(const CBlock& block, const BlockValidationState& stateIn) override {
        if (block.GetHash() != hash)
            return;
        found = true;
        state = stateIn;
    }
};

static RPCHelpMan submitblock()
{
    // We allow 2 arguments for compliance with BIP22. Argument 2 is ignored.
    return RPCHelpMan{"submitblock",
        "\nAttempts to submit new block to network.\n"
        "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n",
        {
            {"hexdata", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hex-encoded block data to submit"},
            {"dummy", RPCArg::Type::STR, RPCArg::DefaultHint{"ignored"}, "dummy value, for compatibility with BIP22. This value is ignored."},
        },
        {
            RPCResult{"If the block was accepted", RPCResult::Type::NONE, "", ""},
            RPCResult{"Otherwise", RPCResult::Type::STR, "", "According to BIP22"},
        },
        RPCExamples{
                    HelpExampleCli("submitblock", "\"mydata\"")
            + HelpExampleRpc("submitblock", "\"mydata\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    std::shared_ptr<CBlock> blockptr = std::make_shared<CBlock>();
    CBlock& block = *blockptr;
    if (!DecodeHexBlk(block, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");
    }

    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    {
        LOCK(cs_main);
        const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock);
        if (pindex) {
            chainman.UpdateUncommittedBlockStructures(block, pindex);
            // No auto-filling of Tensor fields here; tests must provide proper headers/fields.
        }
    }

    bool new_block;
    auto sc = std::make_shared<submitblock_StateCatcher>(block.GetHash());
    CHECK_NONFATAL(chainman.m_options.signals)->RegisterSharedValidationInterface(sc);
    bool accepted = chainman.ProcessNewBlock(blockptr, /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/&new_block);
    CHECK_NONFATAL(chainman.m_options.signals)->UnregisterSharedValidationInterface(sc);
    if (!new_block && accepted) {
        return "duplicate";
    }
    if (!sc->found) {
        return "inconclusive";
    }
    return BIP22ValidationResult(sc->state);
},
    };
}

static RPCHelpMan submitheader()
{
    return RPCHelpMan{"submitheader",
                "\nDecode the given hexdata as a header and submit it as a candidate chain tip if valid."
                "\nThrows when the header is invalid.\n"
                "\nNote: In TensorCash, full block data (including proof blob) must be provided for validation.\n",
                {
                    {"hexdata", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hex-encoded block data (header + proof blob)"},
                },
                RPCResult{
                    RPCResult::Type::NONE, "", "None"},
                RPCExamples{
                    HelpExampleCli("submitheader", "\"aabbcc\"") +
                    HelpExampleRpc("submitheader", "\"aabbcc\"")
                },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    CBlockHeader h;
    CProofBlob proof_sidecar;
    bool has_sidecar = false;
    ChainstateManager& chainman = EnsureAnyChainman(request.context);
    const CBlockIndex* prev_index = nullptr;

    // Try to decode as full block first to extract header + proof sidecar
    CBlock block;
    if (DecodeHexBlk(block, request.params[0].get_str())) {
        // Successfully decoded as full block
        h = block.GetBlockHeader();
        proof_sidecar = block.pow;  // Extract proof blob as sidecar
        has_sidecar = true;

        // Validate that header's hashPoW matches the proof commitment
        {
            LOCK(cs_main);
            prev_index = chainman.m_blockman.LookupBlockIndex(h.hashPrevBlock);
            if (!prev_index) {
                throw JSONRPCError(RPC_VERIFY_ERROR, "Must submit previous header (" + h.hashPrevBlock.GetHex() + ") first");
            }
        }

        // Compute expected proof commitment for validation
        const int nHeight = prev_index->nHeight + 1;
        const bool use_merkle = chainman.GetConsensus().IsVdfSpvActive(nHeight);
        uint256 expected_commitment = block.pow.GetCommitment(use_merkle);

        // Ensure header has correct proof commitment
        if (h.hashPoW != expected_commitment) {
            // Update header with correct commitment
            h.hashPoW = expected_commitment;
        }
    } else if (!DecodeHexBlockHeader(h, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block header decode failed");
    }

    {
        LOCK(cs_main);
        if (!prev_index) {
            prev_index = chainman.m_blockman.LookupBlockIndex(h.hashPrevBlock);
        }
        if (!prev_index) {
            throw JSONRPCError(RPC_VERIFY_ERROR, "Must submit previous header (" + h.hashPrevBlock.GetHex() + ") first");
        }
    }

    // Process header with sidecar validation but WITHOUT storing the full block
    // The header contains hashPoW which commits to the proof, and we have the proof sidecar for validation
    BlockValidationState state;
    bool min_pow_checked{false};

    if (has_sidecar) {
        const CProofBlob& pb = proof_sidecar;
        const int next_height = prev_index ? prev_index->nHeight + 1 : 0;
        const bool require_proof = chainman.GetConsensus().IsVdfVdfVerifyActive(next_height);
        if (pb.vdf.empty()) {
            if (require_proof) {
                throw JSONRPCError(RPC_VERIFY_ERROR, "Proof blob missing VDF data");
            }
            min_pow_checked = true; // pre-activation tolerance
        } else {
            const bool ok = vdf::VerifyAgainstPrevHash(
                h.hashPrevBlock,
                std::span<const uint8_t>(pb.vdf.data(), pb.vdf.size()),
                pb.tick,
                /*discr_bits=*/1024,
                /*recursion=*/0);
            if (!ok) {
                throw JSONRPCError(RPC_VERIFY_ERROR, "VDF verification failed");
            }
            min_pow_checked = true;
        }
    } else {
        throw JSONRPCError(RPC_VERIFY_ERROR, "TensorCash headers require submitting the full block (header + proof blob)");
    }

    // Now feed the header through normal validation with work already checked.
    chainman.ProcessNewBlockHeaders({{h}}, min_pow_checked, state);

    if (state.IsValid()) return UniValue::VNULL;
    if (state.IsError()) {
        throw JSONRPCError(RPC_VERIFY_ERROR, state.ToString());
    }
    throw JSONRPCError(RPC_VERIFY_ERROR, state.GetRejectReason());
},
    };
}

// =============================================================================
// Phase 2a: broker-driven mining RPCs.
//
// `create_mining_work_unit` and `submit_mining_response` give the compute
// broker a direct entry point for fanning work out and submitting solutions
// without depending on the legacy ZMQ PUSH/PULL job loop. The legacy path
// (StartMining + ExtAPI::JobSchedulerLoop / SolutionReceiverLoop) is left
// untouched: operators that use it keep working unchanged. Operators on the
// new path simply do not call StartMining; the broker calls these RPCs
// instead.
//
// Both paths funnel through node::RequestTracker (see node/extapi.h). They
// hold separate trackers, so req_id spaces never collide.
// =============================================================================

namespace {

// Process-local registry of broker-issued mining work units. A separate
// tracker from ExtAPI's so the two paths cannot interfere.
node::RequestTracker g_broker_work_units;

// SHA-256 hash size, mirrors ExtAPI::EXPECTED_HASH_SIZE (private there).
constexpr size_t kBrokerExpectedHashSize = 32;

// Encode the 76-byte block-header prefix (everything except the 4-byte
// nNonce trailer). Mirrors the layout the miner-proxy/broker already
// expects: version(4) | hashPrevBlock(32) | hashMerkleRoot(32) | nTime(4)
// | nBits(4).
std::string EncodeHeaderPrefix(const CBlock& block)
{
    std::vector<unsigned char> header(76);
    WriteLE32(&header[0], static_cast<uint32_t>(block.nVersion));
    std::memcpy(&header[4], block.hashPrevBlock.begin(), 32);
    std::memcpy(&header[36], block.hashMerkleRoot.begin(), 32);
    WriteLE32(&header[68], block.nTime);
    WriteLE32(&header[72], block.nBits);
    return HexStr(header);
}

} // namespace

static RPCHelpMan create_mining_work_unit()
{
    return RPCHelpMan{"create_mining_work_unit",
        "Issue a fresh broker-mining work unit. Each call mutates the coinbase "
        "(payout output + extranonce_tag) so the merkle root rotates and a "
        "distinct header_prefix is returned. The work unit is registered in an "
        "in-process tracker keyed by the returned req_id; "
        "submit_mining_response(req_id, ...) completes the round trip.\n",
        {
            {"network", RPCArg::Type::STR, RPCArg::Optional::NO,
                "Network this work unit is for (e.g. \"main\", \"test\", \"regtest\"). Must match this node's chain."},
            {"payout_script_pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
                "Hex-encoded scriptPubKey for the coinbase payout output."},
            {"extranonce_tag", RPCArg::Type::STR_HEX, RPCArg::Default{""},
                "Hex-encoded extranonce bytes appended to the coinbase scriptSig after the height push. "
                "Different tags rotate the merkle root. The final scriptSig size is bounded by consensus "
                "(coinbase scriptSig must be 2..100 bytes; the request is rejected if exceeded)."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "req_id", "bcore-issued work-unit identifier"},
            {RPCResult::Type::STR_HEX, "header_prefix", "76-byte serialised block header without nNonce"},
            {RPCResult::Type::STR_HEX, "target", "32-byte target derived from the block's nBits"},
            {RPCResult::Type::NUM, "expires_at", "unix epoch seconds when this work unit will be evicted"},
            {RPCResult::Type::NUM, "height", "height of the block being assembled"},
            {RPCResult::Type::STR, "network", "chain name (matches request)"},
        }},
        RPCExamples{
            HelpExampleCli("create_mining_work_unit", "\"regtest\" \"5121...ae\" \"01\"")
            + HelpExampleRpc("create_mining_work_unit", "\"regtest\", \"5121...ae\", \"01\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    Mining& miner = EnsureMining(node);
    ChainstateManager& chainman = EnsureChainman(node);

    const std::string requested_network = request.params[0].get_str();
    const std::string actual_chain = chainman.GetParams().GetChainTypeString();
    if (requested_network != actual_chain) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            strprintf("network mismatch: this node is on '%s' but caller requested '%s'",
                      actual_chain, requested_network));
    }

    // Reject empty BEFORE ParseHexV — otherwise ParseHexV's generic
    // "must be hexadecimal string (not '')" wins and the diagnostic is
    // less specific. ParseHexV still catches non-empty malformed hex.
    if (request.params[1].isStr() && request.params[1].get_str().empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "payout_script_pubkey must be non-empty");
    }
    const std::vector<unsigned char> script_bytes = ParseHexV(request.params[1], "payout_script_pubkey");
    if (script_bytes.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "payout_script_pubkey must be non-empty");
    }
    CScript payout_script(script_bytes.begin(), script_bytes.end());

    std::vector<unsigned char> extranonce_bytes;
    if (!request.params[2].isNull() && !request.params[2].get_str().empty()) {
        extranonce_bytes = ParseHexV(request.params[2], "extranonce_tag");
    }

    // Assemble block via the same path JobSchedulerLoop uses (extapi.cpp:431).
    // miner.createNewBlock acquires its own locks internally; do not hold cs_main here.
    auto blockTemplate = miner.createNewBlock({.coinbase_output_script = payout_script});
    if (!blockTemplate) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "failed to assemble block");
    }
    CBlock block = blockTemplate->getBlock();

    if (block.vtx.empty() || block.vtx[0]->vin.empty()) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "assembled block has no coinbase input");
    }

    // Resolve next height for the rebuilt coinbase scriptSig.
    int height = 0;
    {
        LOCK(cs_main);
        const CBlockIndex* prev_index = chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock);
        height = prev_index ? prev_index->nHeight + 1 : 0;
    }

    // Mutate the coinbase scriptSig: BlockAssembler emits "<height> OP_0"
    // (miner.cpp:200). Replace OP_0 with extranonce_bytes when provided so
    // distinct extranonce_tags rotate the coinbase txid -> merkle root ->
    // header_prefix. This is the fanout primitive (see COMPUTE_BROKER_IMPROV.md).
    {
        CMutableTransaction coinbase_mut(*block.vtx[0]);
        CScript new_scriptsig = CScript() << height;
        if (!extranonce_bytes.empty()) {
            new_scriptsig << extranonce_bytes;
        } else {
            new_scriptsig << OP_0;
        }
        // Enforce coinbase scriptSig consensus rule: 2 <= size <= 100 bytes
        // (consensus/tx_check.cpp:58 "bad-cb-length"). The exact ceiling on
        // extranonce depends on height (CompactSize push for height eats
        // 2-5 bytes), so we check the assembled scriptSig rather than gating
        // the input length up front. Without this, oversize extranonce_tag
        // values would silently mint a work unit no miner could ever submit.
        if (new_scriptsig.size() < 2 || new_scriptsig.size() > 100) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(
                "assembled coinbase scriptSig size %zu is outside consensus range [2, 100]; "
                "reduce extranonce_tag length (height %d uses %zu byte(s) for the height push)",
                new_scriptsig.size(),
                height,
                (CScript() << height).size()));
        }
        coinbase_mut.vin[0].scriptSig = new_scriptsig;
        block.vtx[0] = MakeTransactionRef(std::move(coinbase_mut));
        block.hashMerkleRoot = BlockMerkleRoot(block);
    }

    // Register in broker tracker -> mint req_id.
    const uint32_t req_id = g_broker_work_units.incrementAndStore(block);

    // Compute target from nBits (mirrors mining.cpp:996 idiom).
    bool fNegative = false;
    bool fOverflow = false;
    arith_uint256 bn_target;
    bn_target.SetCompact(block.nBits, &fNegative, &fOverflow);
    if (fNegative || fOverflow || bn_target == 0) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "invalid nBits in assembled block");
    }
    const uint256 target = ArithToUint256(bn_target);

    // RequestTracker uses steady_clock with REQUEST_EXPIRY = 10 minutes
    // (extapi.h:63). Expose the wall-clock equivalent for the broker's lease.
    const int64_t expires_at = GetTime() +
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::minutes(10)).count();

    UniValue result(UniValue::VOBJ);
    result.pushKV("req_id", static_cast<uint64_t>(req_id));
    result.pushKV("header_prefix", EncodeHeaderPrefix(block));
    result.pushKV("target", target.GetHex());
    result.pushKV("expires_at", expires_at);
    result.pushKV("height", height);
    result.pushKV("network", actual_chain);
    return result;
},
    };
}

static RPCHelpMan submit_mining_response()
{
    return RPCHelpMan{"submit_mining_response",
        "Submit a broker-mined solution against a previously-issued work unit. "
        "Validates the MiningResponse FlatBuffer, fills the PoW blob on the "
        "stored block, regenerates hashPoW, runs QuickVerify, and submits via "
        "ProcessNewBlock. Returns a structured result for accept/reject/dedup.\n",
        {
            {"req_id", RPCArg::Type::NUM, RPCArg::Optional::NO,
                "Work-unit identifier returned by create_mining_work_unit."},
            {"mining_response_b64", RPCArg::Type::STR, RPCArg::Optional::NO,
                "Base64-encoded proof::MiningResponse FlatBuffer."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::BOOL, "accepted", "true ONLY when the block is active on the main chain with data; false while Full validation is still in flight or on reject"},
            {RPCResult::Type::STR, "status", "one of: accepted | accepted_pending_connect | rejected | unknown_req_id | already_submitted | invalid_payload | quick_verify_failed"},
            {RPCResult::Type::STR, "block_hash", /*optional=*/true, "hex hash of the submitted block (when reachable)"},
            {RPCResult::Type::STR, "reject_reason", /*optional=*/true, "validation state ToString or decode error"},
        }},
        RPCExamples{
            HelpExampleCli("submit_mining_response", "42 \"AAAA...\"")
            + HelpExampleRpc("submit_mining_response", "42, \"AAAA...\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);

    auto reject = [](const std::string& status, const std::string& reason) {
        UniValue r(UniValue::VOBJ);
        r.pushKV("accepted", false);
        r.pushKV("status", status);
        if (!reason.empty()) r.pushKV("reject_reason", reason);
        return r;
    };

    const int64_t req_id_in = request.params[0].getInt<int64_t>();
    if (req_id_in < 1 || req_id_in >= MAX_REQUEST_ID) {
        return reject("invalid_payload", strprintf("req_id out of range: %d", req_id_in));
    }
    const uint32_t req_id = static_cast<uint32_t>(req_id_in);

    auto raw = DecodeBase64(request.params[1].get_str());
    if (!raw) {
        return reject("invalid_payload", "mining_response_b64 is not valid base64");
    }
    const std::vector<unsigned char>& payload = *raw;
    if (payload.size() < sizeof(flatbuffers::uoffset_t)) {
        return reject("invalid_payload", "MiningResponse payload too small");
    }

    // Verify FIRST: a bogus root offset would make GetRoot return a pointer
    // into untrusted memory; reading any field before VerifyBuffer succeeds
    // is undefined behaviour. Only after VerifyBuffer is it safe to call
    // GetRoot and read fields.
    flatbuffers::Verifier verifier(payload.data(), payload.size());
    if (!verifier.VerifyBuffer<proof::MiningResponse>(nullptr)) {
        return reject("invalid_payload", "MiningResponse FlatBuffer failed verification");
    }
    const proof::MiningResponse* resp = flatbuffers::GetRoot<proof::MiningResponse>(payload.data());
    if (!resp) {
        return reject("invalid_payload", "null MiningResponse root");
    }
    if (resp->req_id() != req_id) {
        return reject("invalid_payload", strprintf(
            "req_id mismatch: rpc=%u flatbuffer=%u", req_id, resp->req_id()));
    }
    if (!resp->pow_blob()) {
        return reject("invalid_payload", "MiningResponse missing pow_blob");
    }
    if (!resp->pow_blob_hash() || resp->pow_blob_hash()->size() != kBrokerExpectedHashSize) {
        return reject("invalid_payload", "MiningResponse pow_blob_hash missing or wrong size");
    }

    // Look up the stored block by req_id.
    auto lookup = g_broker_work_units.getRequestForSolution(req_id);
    if (lookup.state == node::RequestTracker::LookupState::Submitted) {
        return reject("already_submitted", "");
    }
    if (lookup.state == node::RequestTracker::LookupState::Missing || !lookup.block.has_value()) {
        return reject("unknown_req_id", "");
    }

    CBlock block = *lookup.block;
    block.nNonce = resp->nonce();
    block.nAdjBits = resp->adjusted_bits();

    try {
        block.pow.fillFromFB(resp->pow_blob());
    } catch (const std::exception& e) {
        return reject("invalid_payload", strprintf("pow.fillFromFB: %s", e.what()));
    }

    // hashPoW + cumulative_tick: same logic as ExtAPI::ValidateMiningResponse
    // (extapi.cpp:349-377). We MUST regenerate these before ProcessNewBlock,
    // otherwise the stored block carries stale (zero) values.
    int next_height = 0;
    {
        const CBlockIndex* prev_index = WITH_LOCK(::cs_main,
            return chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock));
        next_height = prev_index ? prev_index->nHeight + 1 : 0;
        const bool use_merkle = chainman.GetConsensus().IsVdfSpvActive(next_height);
        block.hashPoW = block.pow.GetCommitment(use_merkle);

        if (prev_index != nullptr) {
            CBlock prev_blk;
            if (chainman.m_blockman.ReadBlock(prev_blk, *prev_index)) {
                block.cumulative_tick = prev_blk.cumulative_tick + block.pow.tick;
            } else {
                block.cumulative_tick = block.pow.tick;
            }
        } else {
            block.cumulative_tick = block.pow.tick;
        }
    }

    // QuickVerify before submission, mirroring SolutionReceiverLoop:452-458.
    QuickVerifier quick_verifier;
    // Version-keyed: enforces the reuse gate iff block.pow.version >= REUSE_GATE_VERSION.
    const VerificationResult qv = quick_verifier.QuickVerify(block.pow);
    if (qv != VerificationResult::Quick_OK) {
        return reject("quick_verify_failed", quick_verifier.GetLastError());
    }

    // Submit through the same path SolutionReceiverLoop uses (extapi.cpp:460-492).
    const uint256 block_hash = block.GetHash();
    auto blockPtr = std::make_shared<const CBlock>(std::move(block));
    auto sc = std::make_shared<submitblock_StateCatcher>(block_hash);
    CHECK_NONFATAL(chainman.m_options.signals)->RegisterSharedValidationInterface(sc);

    bool new_block = false;
    const bool accepted = chainman.ProcessNewBlock(blockPtr, /*force_processing=*/true,
                                                   /*min_pow_checked=*/true, &new_block);

    CHECK_NONFATAL(chainman.m_options.signals)->UnregisterSharedValidationInterface(sc);

    UniValue r(UniValue::VOBJ);
    r.pushKV("block_hash", block_hash.ToString());

    // Report accepted=true ONLY when the block is active on the main chain
    // WITH DATA. A header-only index, a valid-but-side-branch block, or a
    // block whose Full (external_api) validation is still in flight must NOT
    // be reported as accepted: the old contract returned accepted=true the
    // instant async validation was *kicked off*, which told the broker to
    // finalise a wallet payout for a block that had not connected — and if a
    // mid-validation restart (the genesis vanity-onion bug) stranded the body,
    // the chain stayed below that "accepted" height with bogus payout rows.
    auto block_active_with_data = [&]() -> bool {
        LOCK(cs_main);
        const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(block_hash);
        return pindex != nullptr
            && (pindex->nStatus & BLOCK_HAVE_DATA)
            && chainman.ActiveChain().Contains(pindex);
    };
    auto block_failed = [&]() -> bool {
        LOCK(cs_main);
        const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(block_hash);
        return pindex != nullptr && (pindex->nStatus & BLOCK_FAILED_MASK);
    };

    // Full validation is still running for THIS block only when ProcessNewBlock
    // returned no verdict (accepted=false) AND no synchronous BlockChecked fired
    // (sc->found=false) AND the external validation API owns it. A *synchronous*
    // reject sets sc->found and must NOT be treated as in-flight — otherwise a
    // genuine local reject would be mis-reported as accepted_pending_connect.
    const bool async_validation_in_flight =
        !sc->found && chainman.GetConsensus().external_api;

    // Synchronous accept: ProcessNewBlock connected the block into the tree
    // in-line. Decide immediately — there is no async verdict pending. If it is
    // the active tip with data, confirm. If it is valid-but-not-the-tip (the
    // chain advanced between work-unit creation and submission, leaving this a
    // same-height side branch), leave it BOUND as pending: a later reorg may
    // still make it active, and reconciliation owns that transition.
    if (accepted) {
        if (block_active_with_data()) {
            g_broker_work_units.markSubmitted(req_id);
            r.pushKV("accepted", true);
            r.pushKV("status", "accepted");
            return r;
        }
        r.pushKV("accepted", false);
        r.pushKV("status", "accepted_pending_connect");
        return r;
    }

    // Async Full validation in flight: hold the RPC and wait — bounded — for the
    // block to connect to the active chain WITH DATA, or to be marked failed.
    // The broker's submit_timeout_sec (60s) covers this; Full validation has
    // been observed at ~32s.
    if (async_validation_in_flight) {
        using namespace std::chrono_literals;
        constexpr auto kPoll = 200ms;
        constexpr int kMaxIters = 225;  // 225 * 200ms = 45s, inside the 60s broker budget
        for (int i = 0; i < kMaxIters && !chainman.m_interrupt; ++i) {
            if (block_active_with_data()) {
                g_broker_work_units.markSubmitted(req_id);
                r.pushKV("accepted", true);
                r.pushKV("status", "accepted");
                return r;
            }
            if (block_failed()) {
                r.pushKV("accepted", false);
                r.pushKV("status", "rejected");
                r.pushKV("reject_reason", "block failed full validation");
                return r;
            }
            UninterruptibleSleep(kPoll);
        }
        // Still validating after the bounded wait. Do NOT mark the work unit
        // submitted (keep it reconcilable/retriable if validation later fails)
        // and do NOT claim acceptance. The broker maps accepted_pending_connect
        // to submission_unknown and leaves the allocation BOUND until
        // reconciliation confirms the chain tip.
        r.pushKV("accepted", false);
        r.pushKV("status", "accepted_pending_connect");
        return r;
    }

    // Synchronous reject (sc->found), or the !external_api fallback: a genuine
    // reject carrying the real validation state.
    r.pushKV("accepted", false);
    r.pushKV("status", "rejected");
    r.pushKV("reject_reason", sc->found ? sc->state.ToString()
                                        : std::string{"not accepted synchronously and not external_api"});
    return r;
},
    };
}

// Idempotently drop a broker-issued work unit from the tracker so the 50-entry
// cap (extapi.h:85-89) does not silently LRU-evict live work. The broker MUST
// call this on every terminal lease transition (accepted/rejected/expired/
// superseded/send-failed) so its in-flight view matches bcore's tracker
// occupancy. Returning `released=false` for an unknown/already-removed id is
// intentional: it is the success signal for "no-op idempotent cleanup", not
// an error. Range errors are a caller bug and throw, mirroring the
// validate-then-act flow in create_mining_work_unit.
static RPCHelpMan release_mining_work_unit()
{
    return RPCHelpMan{"release_mining_work_unit",
        "Idempotently release a broker-issued work unit. Removes the stored "
        "block from the broker tracker so it stops counting against the "
        "50-entry cap. Returns {req_id, released} where released=false signals "
        "no-op idempotent cleanup (unknown or already-removed req_id), NOT an "
        "error. Use this on every terminal broker-lease transition.\n",
        {
            {"req_id", RPCArg::Type::NUM, RPCArg::Optional::NO,
                "Work-unit identifier returned by create_mining_work_unit."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "req_id", "req_id that was targeted"},
            {RPCResult::Type::BOOL, "released", "true if an entry existed and was erased; false on idempotent no-op"},
        }},
        RPCExamples{
            HelpExampleCli("release_mining_work_unit", "42")
            + HelpExampleRpc("release_mining_work_unit", "42")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    // Range bounds match submit_mining_response (mining.cpp:1501). An
    // out-of-range req_id is a caller bug, not idempotent cleanup —
    // surface it as RPC_INVALID_PARAMETER so the broker sees a hard error.
    const int64_t req_id_in = request.params[0].getInt<int64_t>();
    if (req_id_in < 1 || req_id_in >= MAX_REQUEST_ID) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            strprintf("req_id out of range: %d (must be in [1, %u))", req_id_in, MAX_REQUEST_ID));
    }
    const uint32_t req_id = static_cast<uint32_t>(req_id_in);

    const bool released = g_broker_work_units.remove(req_id);

    UniValue r(UniValue::VOBJ);
    r.pushKV("req_id", static_cast<uint64_t>(req_id));
    r.pushKV("released", released);
    return r;
},
    };
}

void RegisterMiningRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"mining", &getnetworkhashps},
        {"mining", &getmininginfo},
        {"mining", &prioritisetransaction},
        {"mining", &getprioritisedtransactions},
        {"mining", &getblocktemplate},
        {"mining", &submitblock},
        {"mining", &submitheader},
        {"mining", &create_mining_work_unit},
        {"mining", &submit_mining_response},
        {"mining", &release_mining_work_unit},

        {"hidden", &generatetoaddress},
        {"hidden", &generatetodescriptor},
        {"hidden", &generateblock},
        {"hidden", &generate},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
