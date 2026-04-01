#include <common/args.h>
#include <modeldb.h>
#include <leveldb/options.h>
#include <leveldb/write_batch.h>
#include <logging.h>
#include <wallet/rpc/api_model_registration.h>

namespace {
constexpr uint8_t DB_MODEL_PREFIX{'m'};
constexpr uint8_t DB_DEPOSIT_PREFIX{'d'};
constexpr uint8_t DB_BURN_PREFIX{'b'};
constexpr uint8_t DB_VERIFICATION_PREFIX{'v'};
constexpr uint8_t DB_CHALLENGE_PREFIX{'c'};
constexpr uint8_t DB_CHALLENGE_SCHEDULE_PREFIX{'q'};
constexpr uint8_t DB_BLOCK_MODEL_PREFIX{'x'};
constexpr uint8_t DB_MODEL_BLOCK_PREFIX{'y'};
constexpr uint8_t DB_SYNCED_TIP_PREFIX{'s'};

struct ModelKey {
    uint8_t prefix{DB_MODEL_PREFIX};
    uint256 hash;

    ModelKey() = default;
    explicit ModelKey(const uint256& h) : hash(h) {}

    SERIALIZE_METHODS(ModelKey, obj)
    {
        READWRITE(obj.prefix, obj.hash);
    }
};

struct DepositKey {
    uint8_t prefix{DB_DEPOSIT_PREFIX};
    uint256 txid;
    uint32_t vout{0};

    [[maybe_unused]] DepositKey() = default;
    explicit DepositKey(const COutPoint& out) : txid(out.hash), vout(out.n) {}

    SERIALIZE_METHODS(DepositKey, obj)
    {
        READWRITE(obj.prefix, obj.txid, obj.vout);
    }
};

struct BurnKey {
    uint8_t prefix{DB_BURN_PREFIX};
    uint256 txid;
    uint32_t vout{0};

    BurnKey() = default;
    explicit BurnKey(const COutPoint& out) : txid(out.hash), vout(out.n) {}

    SERIALIZE_METHODS(BurnKey, obj)
    {
        READWRITE(obj.prefix, obj.txid, obj.vout);
    }
};

struct VerificationKey {
    uint8_t prefix{DB_VERIFICATION_PREFIX};
    uint32_t height{0};
    uint256 hash;

    VerificationKey() = default;
    VerificationKey(uint32_t h, const uint256& m) : height(h), hash(m) {}

    SERIALIZE_METHODS(VerificationKey, obj)
    {
        READWRITE(obj.prefix, obj.height, obj.hash);
    }
};

struct ChallengeKey {
    uint8_t prefix{DB_CHALLENGE_PREFIX};
    uint256 txid;
    uint32_t vout{0};

    ChallengeKey() = default;
    explicit ChallengeKey(const COutPoint& out) : txid(out.hash), vout(out.n) {}

    SERIALIZE_METHODS(ChallengeKey, obj)
    {
        READWRITE(obj.prefix, obj.txid, obj.vout);
    }
};

struct ChallengeScheduleKey {
    uint8_t prefix{DB_CHALLENGE_SCHEDULE_PREFIX};
    uint32_t height{0};
    uint256 hash;

    ChallengeScheduleKey() = default;
    ChallengeScheduleKey(uint32_t h, const uint256& m) : height(h), hash(m) {}

    SERIALIZE_METHODS(ChallengeScheduleKey, obj)
    {
        READWRITE(obj.prefix, obj.height, obj.hash);
    }
};

struct BlockModelKey {
    uint8_t prefix{DB_BLOCK_MODEL_PREFIX};
    uint256 block_hash;

    BlockModelKey() = default;
    explicit BlockModelKey(const uint256& b) : block_hash(b) {}

    SERIALIZE_METHODS(BlockModelKey, obj)
    {
        READWRITE(obj.prefix, obj.block_hash);
    }
};

struct BlockModelValue {
    uint32_t height{0};
    uint256 model_hash;

    SERIALIZE_METHODS(BlockModelValue, obj)
    {
        READWRITE(obj.height, obj.model_hash);
    }
};

struct ModelBlockKey {
    uint8_t prefix{DB_MODEL_BLOCK_PREFIX};
    uint256 model_hash;
    uint32_t height{0};
    uint256 block_hash;

    ModelBlockKey() = default;
    ModelBlockKey(const uint256& m, uint32_t h, const uint256& b) : model_hash(m), height(h), block_hash(b) {}

    SERIALIZE_METHODS(ModelBlockKey, obj)
    {
        READWRITE(obj.prefix, obj.model_hash, obj.height, obj.block_hash);
    }
};

struct SyncedTipKey {
    uint8_t prefix{DB_SYNCED_TIP_PREFIX};

    SERIALIZE_METHODS(SyncedTipKey, obj)
    {
        READWRITE(obj.prefix);
    }
};

struct SyncedTipValue {
    int32_t height{0};
    uint256 hash{uint256::ZERO};

    SERIALIZE_METHODS(SyncedTipValue, obj)
    {
        READWRITE(obj.height, obj.hash);
    }
};

} // namespace

std::unique_ptr<CModelDB> g_modeldb = nullptr;

CModelDB::CModelDB(const Consensus::Params& consensusParams, size_t cache_size, bool fMemory, bool fWipe)
{
    DBParams params;
    params.cache_bytes = cache_size;
    params.memory_only = fMemory;
    params.wipe_data = fWipe;
    params.path = gArgs.GetDataDirNet() / "modeldb";
    if (gArgs.GetBoolArg("-reindex", false)) {
        fs::remove_all(params.path);
    }
    db = std::make_unique<CDBWrapper>(params);

    if (IsEmpty()) {
        ModelRecord default_record;
        default_record.metadata.model_name = consensusParams.DefaultModelName;
        default_record.metadata.model_commit = consensusParams.DefaultModelCommit;
        default_record.metadata.difficulty = static_cast<int64_t>(consensusParams.ModelDifficultyNormalizer);
        default_record.metadata.cid = consensusParams.DefaultModelCID;
        default_record.status = ModelRegistrationStatus::Registered;
        default_record.deposit_block_hash = consensusParams.hashGenesisBlock;
        default_record.deposit_block_height = 0;
        default_record.commit_block_hash = consensusParams.hashGenesisBlock;
        default_record.commit_block_height = 0;
        default_record.deposit_amount = 0;
        default_record.verification_code = 0;

        const uint256 model_hash = HashSHA256(default_record.metadata.model_name, default_record.metadata.model_commit);
        if (WriteModel(model_hash, default_record)) {
            LogPrintf("[ModelDB] Default model inserted on startup.\n");
        } else {
            LogPrintf("[ModelDB] Failed to write default model.\n");
        }
    }
    LogPrintf("[ModelDB] Initialized database at %s\n", params.path.utf8string());
}

bool CModelDB::WriteModel(const uint256& model_hash, const ModelRecord& record, bool overwrite)
{
    if (!overwrite && Exists(model_hash)) {
        LogPrintf("[ModelDB] Error: Attempted to write duplicated model for %s.\n", model_hash.ToString());
        return false;
    }
    // Argument-misbinding / durability-hygiene fix: CDBWrapper::Write's 3rd
    // parameter is `fSync`, not an "overwrite" flag (LevelDB always overwrites).
    // The previous code passed `overwrite` into that slot, so the deposit path
    // (overwrite=false) wrote with fSync=false. This is NOT the root cause of the
    // observed modeldb corruption — that was a full/incremental rebuild on a
    // pruned datadir silently marking itself synced-to-tip over a hole (see
    // RebuildModelDbFromActiveChain in init.cpp). It is simply correct to fsync
    // these infrequent model-state writes rather than leave their durability to
    // an unrelated, accidental flag.
    return db->Write(ModelKey(model_hash), record, /*fSync=*/true);
}

bool CModelDB::ReadModel(const uint256& model_hash, ModelRecord& record) const
{
    return db->Read(ModelKey(model_hash), record);
}

bool CModelDB::Exists(const uint256& model_hash) const
{
    if (IsEmpty()) {
        return false;
    }
    return db->Exists(ModelKey(model_hash));
}

bool CModelDB::Erase(const uint256& model_hash)
{
    return db->Erase(ModelKey(model_hash));
}

void CModelDB::ForEachModel(const std::function<void(const uint256&, const ModelRecord&)>& callback) const
{
    std::unique_ptr<CDBIterator> it{db->NewIterator()};
    ModelKey seek_key;
    seek_key.hash.SetNull();
    it->Seek(seek_key);
    while (it->Valid()) {
        ModelKey key;
        if (!it->GetKey(key) || key.prefix != DB_MODEL_PREFIX) {
            break;
        }
        ModelRecord rec;
        if (it->GetValue(rec)) {
            callback(key.hash, rec);
        }
        it->Next();
    }
}

bool CModelDB::IsEmpty() const
{
    return db->IsEmpty();
}

bool CModelDB::WriteDepositIndex(const COutPoint& outpoint, const uint256& model_hash)
{
    return db->Write(DepositKey(outpoint), model_hash);
}

bool CModelDB::ReadDepositIndex(const COutPoint& outpoint, uint256& model_hash) const
{
    return db->Read(DepositKey(outpoint), model_hash);
}

bool CModelDB::EraseDepositIndex(const COutPoint& outpoint)
{
    return db->Erase(DepositKey(outpoint));
}

std::optional<uint256> CModelDB::LookupModelByDeposit(const COutPoint& outpoint) const
{
    uint256 model_hash;
    if (ReadDepositIndex(outpoint, model_hash)) {
        return model_hash;
    }
    return std::nullopt;
}

bool CModelDB::WriteBurnIndex(const COutPoint& outpoint, const uint256& model_hash)
{
    return db->Write(BurnKey(outpoint), model_hash);
}

bool CModelDB::ReadBurnIndex(const COutPoint& outpoint, uint256& model_hash) const
{
    return db->Read(BurnKey(outpoint), model_hash);
}

bool CModelDB::EraseBurnIndex(const COutPoint& outpoint)
{
    return db->Erase(BurnKey(outpoint));
}

std::optional<uint256> CModelDB::LookupModelByBurn(const COutPoint& outpoint) const
{
    uint256 model_hash;
    if (ReadBurnIndex(outpoint, model_hash)) {
        return model_hash;
    }
    return std::nullopt;
}

bool CModelDB::WriteChallengeDepositIndex(const COutPoint& outpoint, const uint256& model_hash)
{
    return db->Write(ChallengeKey(outpoint), model_hash, true);
}

bool CModelDB::ReadChallengeDepositIndex(const COutPoint& outpoint, uint256& model_hash) const
{
    return db->Read(ChallengeKey(outpoint), model_hash);
}

bool CModelDB::EraseChallengeDepositIndex(const COutPoint& outpoint)
{
    return db->Erase(ChallengeKey(outpoint));
}

std::optional<uint256> CModelDB::LookupModelByChallengeDeposit(const COutPoint& outpoint) const
{
    uint256 model_hash;
    if (ReadChallengeDepositIndex(outpoint, model_hash)) {
        return model_hash;
    }
    return std::nullopt;
}

bool CModelDB::WriteVerificationSchedule(uint32_t height, const uint256& model_hash, const VerificationValue& value)
{
    return db->Write(VerificationKey(height, model_hash), value, true);
}

bool CModelDB::EraseVerificationSchedule(uint32_t height, const uint256& model_hash)
{
    return db->Erase(VerificationKey(height, model_hash));
}

std::optional<CModelDB::VerificationValue> CModelDB::ReadVerificationSchedule(uint32_t height, const uint256& model_hash) const
{
    VerificationValue value;
    if (db->Read(VerificationKey(height, model_hash), value)) {
        return value;
    }
    return std::nullopt;
}

bool CModelDB::UpdateVerificationSchedule(uint32_t height, const uint256& model_hash, const VerificationValue& value)
{
    return db->Write(VerificationKey(height, model_hash), value, true);
}

std::vector<std::pair<uint256, CModelDB::VerificationValue>> CModelDB::GetVerificationSchedule(uint32_t height) const
{
    std::vector<std::pair<uint256, VerificationValue>> results;
    std::unique_ptr<CDBIterator> it{db->NewIterator()};
    VerificationKey seek(height, uint256::ZERO);
    it->Seek(seek);
    while (it->Valid()) {
        VerificationKey key;
        if (!it->GetKey(key) || key.prefix != DB_VERIFICATION_PREFIX || key.height != height) {
            break;
        }
        VerificationValue value;
        if (it->GetValue(value)) {
            results.emplace_back(key.hash, value);
        }
        it->Next();
    }
    return results;
}

bool CModelDB::WriteChallengeSchedule(uint32_t height, const uint256& model_hash, const ChallengeValue& value)
{
    return db->Write(ChallengeScheduleKey(height, model_hash), value, true);
}

bool CModelDB::EraseChallengeSchedule(uint32_t height, const uint256& model_hash)
{
    return db->Erase(ChallengeScheduleKey(height, model_hash));
}

std::optional<CModelDB::ChallengeValue> CModelDB::ReadChallengeSchedule(uint32_t height, const uint256& model_hash) const
{
    ChallengeValue value;
    if (db->Read(ChallengeScheduleKey(height, model_hash), value)) {
        return value;
    }
    return std::nullopt;
}

bool CModelDB::UpdateChallengeSchedule(uint32_t height, const uint256& model_hash, const ChallengeValue& value)
{
    return db->Write(ChallengeScheduleKey(height, model_hash), value, true);
}

std::vector<std::pair<uint256, CModelDB::ChallengeValue>> CModelDB::GetChallengeSchedule(uint32_t height) const
{
    std::vector<std::pair<uint256, ChallengeValue>> results;
    std::unique_ptr<CDBIterator> it{db->NewIterator()};
    ChallengeScheduleKey seek(height, uint256::ZERO);
    it->Seek(seek);
    while (it->Valid()) {
        ChallengeScheduleKey key;
        if (!it->GetKey(key) || key.prefix != DB_CHALLENGE_SCHEDULE_PREFIX || key.height != height) {
            break;
        }
        ChallengeValue value;
        if (it->GetValue(value)) {
            results.emplace_back(key.hash, value);
        }
        it->Next();
    }
    return results;
}

bool CModelDB::WriteBlockModelIndex(const uint256& block_hash, uint32_t height, const uint256& model_hash)
{
    if (model_hash.IsNull()) {
        return true;
    }
    CDBBatch batch(*db);
    BlockModelValue value;
    value.height = height;
    value.model_hash = model_hash;
    batch.Write(BlockModelKey(block_hash), value);
    batch.Write(ModelBlockKey(model_hash, height, block_hash), uint8_t{1});
    return db->WriteBatch(batch);
}

std::vector<uint256> CModelDB::GetBlocksForModelFromHeight(const uint256& model_hash, uint32_t min_height) const
{
    std::vector<uint256> results;
    std::unique_ptr<CDBIterator> it{db->NewIterator()};
    ModelBlockKey seek(model_hash, 0, uint256::ZERO);
    it->Seek(seek);
    while (it->Valid()) {
        ModelBlockKey key;
        if (!it->GetKey(key) || key.prefix != DB_MODEL_BLOCK_PREFIX || key.model_hash != model_hash) {
            break;
        }
        if (key.height >= min_height) {
            results.push_back(key.block_hash);
        }
        it->Next();
    }
    return results;
}

bool CModelDB::WriteSyncedTip(int height, const uint256& block_hash)
{
    SyncedTipValue value;
    value.height = height;
    value.hash = block_hash;
    return db->Write(SyncedTipKey{}, value, true);
}

bool CModelDB::ReadSyncedTip(int& height, uint256& block_hash) const
{
    SyncedTipValue value;
    if (!db->Read(SyncedTipKey{}, value)) return false;
    height = value.height;
    block_hash = value.hash;
    return true;
}

bool CModelDB::EraseSyncedTip()
{
    return db->Erase(SyncedTipKey{});
}
