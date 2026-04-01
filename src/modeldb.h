// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MODELDB_H
#define BITCOIN_MODELDB_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <dbwrapper.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>
#include <util/fs.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <ios>
#include <vector>

enum class ModelRegistrationStatus : uint8_t {
    PendingDeposit = 0,
    PendingVerification = 1,
    Registered = 2,
    Locked = 3,
    Banned = 4,
};

struct ModelMetadata {
    std::string model_name;
    std::string model_commit;
    int64_t difficulty{0};
    std::string cid;
    std::string extra;

    SERIALIZE_METHODS(ModelMetadata, obj)
    {
        READWRITE(obj.model_name,
                  obj.model_commit,
                  obj.difficulty,
                  obj.cid,
                  obj.extra);
    }
};

struct ModelRecord {
    ModelMetadata metadata;
    ModelRegistrationStatus status{ModelRegistrationStatus::PendingDeposit};
    uint256 deposit_txid{uint256::ZERO};
    uint32_t deposit_vout{0};
    CAmount deposit_amount{0};
    uint160 owner_key_hash{};
    uint256 deposit_block_hash{uint256::ZERO};
    int deposit_block_height{0};
    uint256 commit_txid{uint256::ZERO};
    uint256 commit_block_hash{uint256::ZERO};
    int commit_block_height{0};
    uint256 burn_txid{uint256::ZERO};
    uint32_t burn_vout{0};
    int burn_block_height{0};
    uint32_t verification_code{0};
    std::string verification_details;
    int verification_event_height{0};
    uint32_t successful_commit_count{0};
    uint256 challenge_block_hash{uint256::ZERO};
    uint256 challenge_deposit_txid{uint256::ZERO};
    uint32_t challenge_deposit_vout{0};
    int challenge_deposit_height{0};
    uint32_t challenge_commit_count{0};
    int challenge_verdict_height{0};

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ::Serialize(s, metadata);
        uint8_t status_u8 = static_cast<uint8_t>(status);
        ::Serialize(s, status_u8);
        ::Serialize(s, deposit_txid);
        ::Serialize(s, deposit_vout);
        ::Serialize(s, deposit_amount);
        ::Serialize(s, owner_key_hash);
        ::Serialize(s, deposit_block_hash);
        ::Serialize(s, deposit_block_height);
        ::Serialize(s, commit_txid);
        ::Serialize(s, commit_block_hash);
        ::Serialize(s, commit_block_height);
        ::Serialize(s, burn_txid);
        ::Serialize(s, burn_vout);
        ::Serialize(s, burn_block_height);
        ::Serialize(s, verification_code);
        ::Serialize(s, verification_details);
        ::Serialize(s, verification_event_height);
        ::Serialize(s, successful_commit_count);
        ::Serialize(s, challenge_block_hash);
        ::Serialize(s, challenge_deposit_txid);
        ::Serialize(s, challenge_deposit_vout);
        ::Serialize(s, challenge_deposit_height);
        ::Serialize(s, challenge_commit_count);
        ::Serialize(s, challenge_verdict_height);
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        ::Unserialize(s, metadata);
        uint8_t status_u8{0};
        ::Unserialize(s, status_u8);
        status = static_cast<ModelRegistrationStatus>(status_u8);
        ::Unserialize(s, deposit_txid);
        ::Unserialize(s, deposit_vout);
        ::Unserialize(s, deposit_amount);
        ::Unserialize(s, owner_key_hash);
        ::Unserialize(s, deposit_block_hash);
        ::Unserialize(s, deposit_block_height);
        ::Unserialize(s, commit_txid);
        ::Unserialize(s, commit_block_hash);
        ::Unserialize(s, commit_block_height);
        ::Unserialize(s, burn_txid);
        ::Unserialize(s, burn_vout);
        ::Unserialize(s, burn_block_height);
        ::Unserialize(s, verification_code);
        ::Unserialize(s, verification_details);
        verification_event_height = 0;
        try {
            ::Unserialize(s, verification_event_height);
        } catch (const std::ios_base::failure&) {
            s.clear();
            verification_event_height = 0;
        }
        successful_commit_count = 0;
        try {
            ::Unserialize(s, successful_commit_count);
        } catch (const std::ios_base::failure&) {
            s.clear();
            successful_commit_count = 0;
        }
        challenge_block_hash.SetNull();
        challenge_deposit_txid.SetNull();
        challenge_deposit_vout = 0;
        challenge_deposit_height = 0;
        challenge_commit_count = 0;
        challenge_verdict_height = 0;
        try {
            ::Unserialize(s, challenge_block_hash);
            ::Unserialize(s, challenge_deposit_txid);
            ::Unserialize(s, challenge_deposit_vout);
            ::Unserialize(s, challenge_deposit_height);
            ::Unserialize(s, challenge_commit_count);
            ::Unserialize(s, challenge_verdict_height);
        } catch (const std::ios_base::failure&) {
            s.clear();
            challenge_block_hash.SetNull();
            challenge_deposit_txid.SetNull();
            challenge_deposit_vout = 0;
            challenge_deposit_height = 0;
            challenge_commit_count = 0;
            challenge_verdict_height = 0;
        }
    }

    std::string ToString() const
    {
        return strprintf(
            "ModelRecord(name=%s, commit=%s, difficulty=%d, status=%u, deposit_txid=%s, deposit_vout=%u, commit_txid=%s, commit_height=%d, burn_txid=%s, burn_vout=%u, burn_height=%d, commits=%u, verify_height=%d)",
            metadata.model_name,
            metadata.model_commit,
            metadata.difficulty,
            static_cast<unsigned>(status),
            deposit_txid.ToString(),
            deposit_vout,
            commit_txid.ToString(),
            commit_block_height,
            burn_txid.ToString(),
            burn_vout,
            burn_block_height,
            successful_commit_count,
            verification_event_height);
    }
};

class CModelDB {
private:
    std::unique_ptr<CDBWrapper> db;

public:
    struct VerificationValue {
        uint8_t state{0};
        bool has_snapshot{false};
        ModelRecord snapshot;

        SERIALIZE_METHODS(VerificationValue, obj)
        {
            READWRITE(obj.state, obj.has_snapshot);
            if (obj.has_snapshot) {
                READWRITE(obj.snapshot);
            }
        }
    };

    struct ChallengeValue {
        uint8_t state{0};
        COutPoint deposit_outpoint{};
        bool has_snapshot{false};
        ModelRecord snapshot;
        bool has_verdict_snapshot{false};
        ModelRecord verdict_snapshot;

        template <typename Stream>
        void Serialize(Stream& s) const
        {
            ::Serialize(s, state);
            ::Serialize(s, deposit_outpoint);
            ::Serialize(s, has_snapshot);
            if (has_snapshot) {
                ::Serialize(s, snapshot);
            }
            ::Serialize(s, has_verdict_snapshot);
            if (has_verdict_snapshot) {
                ::Serialize(s, verdict_snapshot);
            }
        }

        template <typename Stream>
        void Unserialize(Stream& s)
        {
            ::Unserialize(s, state);
            ::Unserialize(s, deposit_outpoint);
            ::Unserialize(s, has_snapshot);
            if (has_snapshot) {
                ::Unserialize(s, snapshot);
            }
            has_verdict_snapshot = false;
            try {
                ::Unserialize(s, has_verdict_snapshot);
                if (has_verdict_snapshot) {
                    ::Unserialize(s, verdict_snapshot);
                }
            } catch (const std::ios_base::failure&) {
                s.clear();
                has_verdict_snapshot = false;
            }
        }
    };

    explicit CModelDB(const Consensus::Params& consensusParams, size_t cache_size = 1 << 20, bool fMemory = false, bool fWipe = false);

    bool WriteModel(const uint256& model_hash, const ModelRecord& record, bool overwrite = true);
    bool ReadModel(const uint256& model_hash, ModelRecord& record) const;
    bool Exists(const uint256& model_hash) const;
    bool Erase(const uint256& model_hash);
    void ForEachModel(const std::function<void(const uint256&, const ModelRecord&)>& callback) const;
    bool IsEmpty() const;

    bool WriteDepositIndex(const COutPoint& outpoint, const uint256& model_hash);
    bool ReadDepositIndex(const COutPoint& outpoint, uint256& model_hash) const;
    bool EraseDepositIndex(const COutPoint& outpoint);
    std::optional<uint256> LookupModelByDeposit(const COutPoint& outpoint) const;

    bool WriteBurnIndex(const COutPoint& outpoint, const uint256& model_hash);
    bool ReadBurnIndex(const COutPoint& outpoint, uint256& model_hash) const;
    bool EraseBurnIndex(const COutPoint& outpoint);
    std::optional<uint256> LookupModelByBurn(const COutPoint& outpoint) const;

    bool WriteChallengeDepositIndex(const COutPoint& outpoint, const uint256& model_hash);
    bool ReadChallengeDepositIndex(const COutPoint& outpoint, uint256& model_hash) const;
    bool EraseChallengeDepositIndex(const COutPoint& outpoint);
    std::optional<uint256> LookupModelByChallengeDeposit(const COutPoint& outpoint) const;

    bool WriteVerificationSchedule(uint32_t height, const uint256& model_hash, const VerificationValue& value);
    bool EraseVerificationSchedule(uint32_t height, const uint256& model_hash);
    std::optional<VerificationValue> ReadVerificationSchedule(uint32_t height, const uint256& model_hash) const;
    bool UpdateVerificationSchedule(uint32_t height, const uint256& model_hash, const VerificationValue& value);
    std::vector<std::pair<uint256, VerificationValue>> GetVerificationSchedule(uint32_t height) const;

    bool WriteChallengeSchedule(uint32_t height, const uint256& model_hash, const ChallengeValue& value);
    bool EraseChallengeSchedule(uint32_t height, const uint256& model_hash);
    std::optional<ChallengeValue> ReadChallengeSchedule(uint32_t height, const uint256& model_hash) const;
    bool UpdateChallengeSchedule(uint32_t height, const uint256& model_hash, const ChallengeValue& value);
    std::vector<std::pair<uint256, ChallengeValue>> GetChallengeSchedule(uint32_t height) const;

    bool WriteBlockModelIndex(const uint256& block_hash, uint32_t height, const uint256& model_hash);
    std::vector<uint256> GetBlocksForModelFromHeight(const uint256& model_hash, uint32_t min_height) const;

    bool WriteSyncedTip(int height, const uint256& block_hash);
    bool ReadSyncedTip(int& height, uint256& block_hash) const;
    bool EraseSyncedTip();
};

extern std::unique_ptr<CModelDB> g_modeldb;

#endif // BITCOIN_MODELDB_H
