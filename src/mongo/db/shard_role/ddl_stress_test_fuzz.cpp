// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/initializer.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/shard_role/shard_catalog/commit_database_metadata_locally.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_role_loop.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/scopeguard.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

namespace {

// TODO SERVER-130010: the following is arguably a hack since the binaries produced by the existing
// mongo fuzztest integration don't support setting flags meant for centipede (the fuzzing engine).
//
// The intent of the following snippet is to disable the "Per-batch timeout" error in order to
// reduce false positives.

// In centipede runner mode CENTIPEDE_RUNNER_FLAGS is set in the environment by
// the engine before the subprocess binary starts. We append
// :ignore_timeout_reports: here (init_priority 101) before GlobalRunnerState
// reads those flags (init_priority 200). In engine mode the variable is absent,
// so we leave it alone — setting it would make IsCentipedeRunner() return true
// and turn the engine binary into a runner.
struct IgnoreTimeoutReportsSetter {
    IgnoreTimeoutReportsSetter() {
        const char* existing = getenv("CENTIPEDE_RUNNER_FLAGS");
        if (!existing)
            return;
        std::string flags = existing;
        if (flags.empty() || flags.back() != ':')
            flags += ':';
        flags += "ignore_timeout_reports:";
        setenv("CENTIPEDE_RUNNER_FLAGS", flags.c_str(), /*overwrite=*/1);
    }
};

IgnoreTimeoutReportsSetter kIgnoreTimeoutReportsSetter __attribute__((init_priority(101)));
}  // namespace

namespace mongo {

// These functions are here so that ADL works properly in order to print the DDL fields whenever the
// fuzzer finds a failure.
template <typename Sink>
void AbslStringify(Sink& sink, const NamespaceString& value) {
    absl::Format(&sink, "\"%s\"_nss", value.toStringForErrorMsg());
}

template <typename Sink>
void AbslStringify(Sink& sink, const BSONObj& value) {
    absl::Format(&sink, "\"%s\"_bson", value.toString());
}

template <typename Sink>
void AbslStringify(Sink& sink, const ShardRef& value) {
    absl::Format(&sink, "ShardRef{\"%s\"}", value.toString());
}

namespace {

using namespace fuzztest;

const NamespaceString kTargetNss = NamespaceString::createNamespaceString_forTest("test.target");
const NamespaceString kOtherColl1Nss =
    NamespaceString::createNamespaceString_forTest("test.other1");
const NamespaceString kOtherColl2Nss =
    NamespaceString::createNamespaceString_forTest("test.other2");

const auto kNamespacesToTest = std::to_array({kTargetNss, kOtherColl1Nss, kOtherColl2Nss});

auto ArbitraryNamespace() {
    return ElementOf<NamespaceString>(kNamespacesToTest);
}

const ShardRef kThisShard{"myShardName"};
const ShardRef kOtherShard1{"other1"};
const ShardRef kOtherShard2{"other2"};

auto ArbitraryShardRef() {
    return ElementOf<ShardRef>({kThisShard, kOtherShard1, kOtherShard2});
}

struct ClearMetadataDDL {
    NamespaceString nss;

    static auto arbitrary() {
        return StructOf<ClearMetadataDDL>(ArbitraryNamespace());
    }
};

enum class ShardingCollType { kUntracked, kUnsplittable, kSharded };

struct Chunk {
    BSONObj min;
    BSONObj max;
    ShardRef owner;
};

auto ArbitraryUnsplittableChunk() {
    return Map(
        [](const auto& shardRef) {
            return std::vector<Chunk>{
                Chunk{BSON("_id" << MINKEY), BSON("_id" << MAXKEY), shardRef}};
        },
        ArbitraryShardRef());
}

auto ArbitraryMultipleChunks(int maxSplitPoints) {
    auto arbSplitPointsWithOwners = FlatMap(
        [](auto splitPoints) {
            std::sort(splitPoints.begin(), splitPoints.end());
            return PairOf(Just(splitPoints),
                          VectorOf(ArbitraryShardRef()).WithSize(splitPoints.size()));
        },
        NonEmpty(UniqueElementsVectorOf(Arbitrary<int>()).WithMaxSize(maxSplitPoints)));
    auto arbMinKeyOwner = ArbitraryShardRef();
    auto arbChunks = Map(
        [](auto splitPointsAndOwners, const auto& minKeyShard) {
            std::vector<Chunk> results;
            auto& [splitPoints, owners] = splitPointsAndOwners;
            if (splitPoints.size() >= 2) {
                for (int i = 0; i < splitPoints.size() - 1; i++) {
                    auto minKey = BSON("_id" << splitPoints[i]);
                    auto maxKey = BSON("_id" << splitPoints[i + 1]);
                    auto shardRef = owners[i];
                    results.emplace_back(minKey, maxKey, shardRef);
                }
            }
            // Add the MaxKey chunk
            results.emplace_back(
                BSON("_id" << splitPoints.back()), BSON("_id" << MAXKEY), owners.back());
            // Add the MinKey chunk
            results.emplace_back(
                BSON("_id" << MINKEY), BSON("_id" << splitPoints.front()), minKeyShard);
            return results;
        },
        arbSplitPointsWithOwners,
        arbMinKeyOwner);
    return arbChunks;
}

template <typename Sink>
void AbslStringify(Sink& sink, const Chunk& value) {
    absl::Format(&sink, "Chunk{%v, %v, %v}", value.min, value.max, value.owner);
}

template <typename Sink>
void AbslStringify(Sink& sink, const std::vector<Chunk>& value) {
    absl::Format(&sink, "{");
    for (const auto& chunk : value) {
        absl::Format(&sink, "%v, ", chunk);
    }
    absl::Format(&sink, "}");
}

struct CreateDDL {
    NamespaceString nss;
    ShardingCollType shardingType;
    std::vector<Chunk> chunks;

    static auto arbitrary() {
        auto arbCollectionType = ElementOf<ShardingCollType>({ShardingCollType::kUntracked,
                                                              ShardingCollType::kUnsplittable,
                                                              ShardingCollType::kSharded});
        auto arbTargetCollection = ArbitraryNamespace();
        auto arbChunks = ArbitraryMultipleChunks(16);
        auto arbUnsplittableChunk = ArbitraryUnsplittableChunk();
        return FlatMap(
            [](const ShardingCollType collType,
               const auto& nss,
               auto chunks,
               auto unsplittableChunk) {
                switch (collType) {
                    case ShardingCollType::kUntracked:
                        return StructOf<CreateDDL>(
                            Just(nss), Just(collType), Just(std::vector<Chunk>{}));
                    case ShardingCollType::kUnsplittable:
                        return StructOf<CreateDDL>(
                            Just(nss), Just(collType), Just(std::move(unsplittableChunk)));
                    case ShardingCollType::kSharded:
                        return StructOf<CreateDDL>(
                            Just(nss), Just(collType), Just(std::move(chunks)));
                }
            },
            arbCollectionType,
            arbTargetCollection,
            OneOf(arbChunks, arbUnsplittableChunk),  // Either 1 chunk or multiple chunks
            arbUnsplittableChunk);
    }
};

struct DropDDL {
    NamespaceString nss;

    static auto arbitrary() {
        return StructOf<DropDDL>(ArbitraryNamespace());
    }
};

struct RenameDDL {
    NamespaceString from;
    NamespaceString to;

    bool isUpgradingFCV;

    static auto arbitrary() {
        // Skip no-op renames
        return Filter(
            [](const auto& renameOp) { return renameOp.from != renameOp.to; },
            StructOf<RenameDDL>(ArbitraryNamespace(), ArbitraryNamespace(), Arbitrary<bool>()));
    }
};

struct MovePrimaryDDL {
    ShardRef to;

    static auto arbitrary() {
        return StructOf<MovePrimaryDDL>(ArbitraryShardRef());
    }
};

struct SplitChunkDDL {
    int key;
    NamespaceString nss;

    static auto arbitrary() {
        return StructOf<SplitChunkDDL>(Arbitrary<int>(), ArbitraryNamespace());
    }
};

struct MergeChunkDDL {
    int from;
    int to;
    NamespaceString nss;

    static auto arbitrary() {
        auto fromTo = UniqueElementsVectorOf(Arbitrary<int>()).WithSize(2);
        return FlatMap(
            [](const auto& fromTo) {
                const auto from = fromTo[0];
                const auto to = fromTo[1];
                if (from < to) {
                    return StructOf<MergeChunkDDL>(Just(from), Just(to), ArbitraryNamespace());
                } else {
                    return StructOf<MergeChunkDDL>(Just(to), Just(from), ArbitraryNamespace());
                }
            },
            fromTo);
    }
};

struct MoveChunkDDL {
    int key;
    ShardRef destination;
    NamespaceString nss;

    static auto arbitrary() {
        return StructOf<MoveChunkDDL>(Arbitrary<int>(), ArbitraryShardRef(), ArbitraryNamespace());
    }
};

struct ReshardCollectionDDL {
    NamespaceString nss;
    std::vector<Chunk> newChunks;
    bool isUpgrading;

    static auto arbitrary() {
        auto nss = ArbitraryNamespace();
        auto arbChunks = OneOf(ArbitraryMultipleChunks(16), ArbitraryUnsplittableChunk());
        return StructOf<ReshardCollectionDDL>(nss, arbChunks, Arbitrary<bool>());
    }
};

struct MoveCollectionDDL {
    NamespaceString nss;
    ShardRef to;
    bool isUpgrading;

    static auto arbitrary() {
        return StructOf<MoveCollectionDDL>(
            ArbitraryNamespace(), ArbitraryShardRef(), Arbitrary<bool>());
    }
};


using DDL = std::variant<ClearMetadataDDL,
                         CreateDDL,
                         DropDDL,
                         RenameDDL,
                         MovePrimaryDDL,
                         SplitChunkDDL,
                         MergeChunkDDL,
                         MoveChunkDDL,
                         ReshardCollectionDDL,
                         MoveCollectionDDL>;

/**
 * A mocking client that supports transactional updates to the returned contents. This class is
 * meant to represent and abstract a Sharding Catalog present on the CSRS.
 */
class MockCatalogClient final : public ShardingCatalogClientMock {
public:
    using ShardingCatalogClientMock::ShardingCatalogClientMock;

    MockCatalogClient(const MockCatalogClient&) = delete;
    MockCatalogClient(MockCatalogClient&&) = delete;

    CollectionType getCollection(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 repl::ReadConcernLevel readConcernLevel) override {
        std::lock_guard lk{_mutex};

        const auto it =
            std::find_if(_collsAndChunks.begin(), _collsAndChunks.end(), [&](const auto& entry) {
                return entry.first.getNss() == nss;
            });
        uassert(ErrorCodes::NamespaceNotFound,
                "Collection not found in mock",
                it != _collsAndChunks.end());
        return it->first;
    }

    CollectionType getCollection(OperationContext* opCtx,
                                 const UUID& uuid,
                                 repl::ReadConcernLevel readConcernLevel) override {
        std::lock_guard lk{_mutex};

        const auto it =
            std::find_if(_collsAndChunks.begin(), _collsAndChunks.end(), [&](const auto& entry) {
                return entry.first.getUuid() == uuid;
            });
        uassert(ErrorCodes::NamespaceNotFound,
                "Collection not found in mock",
                it != _collsAndChunks.end());
        return it->first;
    }

    // This getChunks code is only called by the internal fetchOwnedChunks method in the
    // authoritatiuve shard functions so we only have to handle that call here where we know the
    // exact query used.
    StatusWith<std::vector<ChunkType>> getChunks(
        OperationContext* opCtx,
        const BSONObj& filter,
        const BSONObj& sort,
        boost::optional<int> limit,
        repl::OpTime* opTime,
        const OID& epoch,
        const Timestamp& timestamp,
        repl::ReadConcernLevel readConcern,
        const boost::optional<BSONObj>& hint = boost::none) override {

        std::lock_guard lk{_mutex};

        const auto uuid = invariantStatusOK(UUID::parse(filter[ChunkType::collectionUUID.name()]));
        const auto conditions = filter["$or"].Array();
        const auto shardRef = ShardRef{conditions[0][ChunkType::shard.name()].String()};

        std::vector<ChunkType> results;
        for (const auto& [coll, chunks] : _collsAndChunks) {
            if (coll.getUuid() != uuid) {
                continue;
            }
            for (const auto& chunk : chunks) {
                if (chunk.getShard() == shardRef) {
                    results.emplace_back(chunk);
                    continue;
                }
                auto inHistory = std::any_of(
                    chunk.getHistory().begin(),
                    chunk.getHistory().end(),
                    [&](const ChunkHistory& elem) { return elem.getShard() == shardRef; });
                if (inHistory) {
                    results.emplace_back(chunk);
                    continue;
                }
            }
        }

        std::sort(results.begin(), results.end(), [](const ChunkType& left, const auto& right) {
            return SimpleBSONObjComparator::kInstance.evaluate(left.getMin() < right.getMin());
        });

        return results;
    }

    // Ignore sinceVersion and always returns everything for simplicity.
    std::pair<CollectionType, std::vector<ChunkType>> getCollectionAndChunks(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkVersion& sinceVersion,
        const repl::ReadConcernArgs& readConcern) override {
        std::lock_guard lk{_mutex};

        const auto it = std::find_if(_collsAndChunks.begin(),
                                     _collsAndChunks.end(),
                                     [&](const auto& pair) { return pair.first.getNss() == nss; });

        uassert(ErrorCodes::NamespaceNotFound,
                "Collection doesn't exist in sharding catalog",
                it != _collsAndChunks.end());
        return *it;
    }

    DatabaseType getDatabase(OperationContext* opCtx,
                             const DatabaseName& db,
                             repl::ReadConcernLevel readConcernLevel) override {
        std::lock_guard lk{_mutex};
        invariant(db == _dbEntry.getDbName());
        return _dbEntry;
    }

    repl::OpTimeWith<std::vector<ShardType>> getAllShards(OperationContext* opCtx,
                                                          repl::ReadConcernLevel readConcern,
                                                          BSONObj filter) override {
        std::vector<ShardType> result = {
            ShardType{kThisShard.getShardId().toString(), boost::none, "localhost-1"},
            ShardType{kOtherShard1.getShardId().toString(), boost::none, "localhost-2"},
            ShardType{kOtherShard2.getShardId().toString(), boost::none, "localhost-3"},
        };
        return repl::OpTimeWith{std::move(result)};
    }

    struct UpdatePass {
    private:
        UpdatePass() = default;
        friend MockCatalogClient;
    };

    void setCollectionMetadata(const UpdatePass& p,
                               CollectionType coll,
                               std::vector<ChunkType> chunks) {
        eraseCollectionMetadata(p, coll.getNss());
        eraseCollectionMetadata(p, coll.getUuid());
        _collsAndChunks.emplace_back(std::make_pair(std::move(coll), std::move(chunks)));
    }

    void eraseCollectionMetadata(const UpdatePass&, const NamespaceString& nss) {
        std::erase_if(_collsAndChunks, [&](const auto& pair) {
            const auto& [existingColl, _] = pair;
            return existingColl.getNss() == nss;
        });
    }

    void eraseCollectionMetadata(const UpdatePass&, const UUID& uuid) {
        std::erase_if(_collsAndChunks, [&](const auto& pair) {
            const auto& [existingColl, _] = pair;
            return existingColl.getUuid() == uuid;
        });
    }

    DatabaseType getDatabase(const UpdatePass&) {
        return _dbEntry;
    }

    void setDatabaseEntry(const UpdatePass&, DatabaseType dbEntry) {
        _dbEntry = std::move(dbEntry);
    }

    std::pair<CollectionType, std::vector<ChunkType>> getCollectionMetadata(
        const UpdatePass&, const NamespaceString& nss) {
        const auto it =
            std::find_if(_collsAndChunks.begin(), _collsAndChunks.end(), [&](const auto& entry) {
                return entry.first.getNss() == nss;
            });
        invariant(it != _collsAndChunks.end());
        return *it;
    }

    template <typename F>
    auto updateAtomically(F&& f) {
        std::unique_lock lk{_mutex};
        return f(*this, UpdatePass{});
    }

private:
    std::vector<std::pair<CollectionType, std::vector<ChunkType>>> _collsAndChunks;
    DatabaseType _dbEntry;
    std::mutex _mutex;
};

class ShardServerFixture : public ShardServerTestFixture {
public:
    MockCatalogClient* mockCatalogClient() {
        return _mockCatalogClient;
    }

    OperationContext* getFixtureOpCtx() const {
        return operationContext();
    }

protected:
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        auto client = std::make_unique<MockCatalogClient>();
        _mockCatalogClient = client.get();
        return client;
    }

private:
    // This method is virtual since it's meant to be used by the unit test framework. However, this
    // fixture is re-used for the fuzzing test which does implement testing on its own. Therefore
    // there's no issue here to leave this blank.
    void TestBody() override {};

    MockCatalogClient* _mockCatalogClient = nullptr;
};

class DDLStateMachine {
private:
    Timestamp nextClusterTime() {
        auto oldValue = currentClusterTime;
        currentClusterTime = Timestamp(oldValue.getSecs() + 1, oldValue.getInc());
        return oldValue;
    }

    void executeOperation(OperationContext* opCtx, const DDL& ddl) {
        std::visit([&](const auto& op) { return executeDDL(opCtx, op); }, ddl);
    }

    void executeDDL(OperationContext* opCtx, const CreateDDL& op) {
        if (currentCatalog.contains(op.nss)) {
            return;
        }

        // Not an arbitrary because it doesn't really matter to fuzz it.
        const auto uuid = UUID::gen();

        ON_BLOCK_EXIT([&] {
            currentCatalog.emplace(std::piecewise_construct,
                                   std::forward_as_tuple(op.nss),
                                   std::forward_as_tuple(uuid, op.shardingType));
        });

        bool isUntracked = op.shardingType == ShardingCollType::kUntracked;

        if (isUntracked) {
            // Untracked collections do not have any sharding catalog manipulations.
            return;
        }

        const auto now = Date_t::now();
        Timestamp collTimestamp = nextClusterTime();

        CollectionType collEntry{
            op.nss, OID::gen(), collTimestamp, now, uuid, KeyPattern{BSON("_id" << 1)}};
        if (op.shardingType == ShardingCollType::kUnsplittable) {
            collEntry.setUnsplittable(true);
        }
        std::vector<ChunkType> chunks;
        auto chunkVersion =
            ChunkVersion{CollectionGeneration{collEntry.getEpoch(), collEntry.getTimestamp()},
                         CollectionPlacement{1, 0}};
        for (const auto& chunk : op.chunks) {
            auto& ct = chunks.emplace_back(
                uuid, ChunkRange{chunk.min, chunk.max}, chunkVersion, chunk.owner);
            ct.setName(OID::gen());
            ct.setOnCurrentShardSince(collTimestamp);
            ct.setHistory(std::vector<ChunkHistory>{ChunkHistory{collTimestamp, chunk.owner}});
            chunkVersion.incMajor();
        }

        static const auto csReason = BSON("reason" << "create");

        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->enterCriticalSectionCatchUpPhase(opCtx, csReason);
            scopedCsr->enterCriticalSectionCommitPhase(opCtx, csReason);
        }
        ON_BLOCK_EXIT([&] {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->exitCriticalSection(opCtx, csReason);
        });

        actualFixture->mockCatalogClient()->updateAtomically([&](auto& mockClient, auto pass) {
            mockClient.setCollectionMetadata(pass, std::move(collEntry), std::move(chunks));
        });

        shard_catalog_commit::commitCollectionMetadataLocally(opCtx, op.nss, isCurrentPrimary);
    }

    void executeDDL(OperationContext* opCtx, const DropDDL& op) {
        if (!currentCatalog.contains(op.nss)) {
            return;
        }

        ON_BLOCK_EXIT([&] { currentCatalog.erase(op.nss); });

        bool isTracked = currentCatalog.at(op.nss).collType != ShardingCollType::kUntracked;

        static const auto csReason = BSON("reason" << "drop");

        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->enterCriticalSectionCatchUpPhase(opCtx, csReason);
            scopedCsr->enterCriticalSectionCommitPhase(opCtx, csReason);
        }

        ON_BLOCK_EXIT([&] {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->exitCriticalSection(opCtx, csReason);
        });

        // The real coordinator does the check after taking the critical section so we do the same
        // here.
        if (!isTracked) {
            // Untracked collections don't have any sharding catalog management things to do.
            return;
        }

        auto shardingCatalog = actualFixture->mockCatalogClient();
        const auto collUuid = shardingCatalog->getCollection(opCtx, op.nss, {}).getUuid();

        shardingCatalog->updateAtomically(
            [&](auto& mockClient, auto pass) { mockClient.eraseCollectionMetadata(pass, op.nss); });

        shard_catalog_commit::commitDropCollectionLocally(opCtx, op.nss, collUuid);
    }

    void executeDDL(OperationContext* opCtx, const RenameDDL& op) {
        if (!currentCatalog.contains(op.from)) {
            return;
        }

        ON_BLOCK_EXIT([&] {
            auto fromValue = currentCatalog.at(op.from);
            currentCatalog.erase(op.to);
            currentCatalog.erase(op.from);
            currentCatalog.emplace(std::piecewise_construct,
                                   std::forward_as_tuple(op.to),
                                   std::forward_as_tuple(std::move(fromValue)));
        });

        boost::optional<std::pair<CollectionType, std::vector<ChunkType>>> existingDataFrom;
        boost::optional<std::pair<CollectionType, std::vector<ChunkType>>> existingDataTo;

        auto shardCatalog = actualFixture->mockCatalogClient();

        if (currentCatalog.at(op.from).collType != ShardingCollType::kUntracked) {
            existingDataFrom = shardCatalog->getCollectionAndChunks(opCtx, op.from, {}, {});
        }
        if (currentCatalog.contains(op.to) &&
            currentCatalog.at(op.to).collType != ShardingCollType::kUntracked) {
            existingDataTo = shardCatalog->getCollectionAndChunks(opCtx, op.to, {}, {});
        }

        static const auto csReason = BSON("reason" << "rename");
        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.from);
            scopedCsr->enterCriticalSectionCatchUpPhase(opCtx, csReason);
            scopedCsr->enterCriticalSectionCommitPhase(opCtx, csReason);
        }
        ON_BLOCK_EXIT([&] {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.from);
            scopedCsr->exitCriticalSection(opCtx, csReason);
        });
        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.to);
            scopedCsr->enterCriticalSectionCatchUpPhase(opCtx, csReason);
            scopedCsr->enterCriticalSectionCommitPhase(opCtx, csReason);
        }
        ON_BLOCK_EXIT([&] {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.to);
            scopedCsr->exitCriticalSection(opCtx, csReason);
        });

        shardCatalog->updateAtomically([&](auto& mockClient, auto pass) {
            // Remove the data from the replaced collection since it will cease to exist.
            mockClient.eraseCollectionMetadata(pass, op.to);

            // If the "from" collection was tracked then "to" will now consist of the "from"
            // sharding metadata with the new name.
            if (existingDataFrom) {
                mockClient.eraseCollectionMetadata(pass, op.from);
                auto newCollEntry = existingDataFrom->first;
                newCollEntry.setNss(op.to);
                mockClient.setCollectionMetadata(pass, newCollEntry, existingDataFrom->second);
            }
        });

        shard_catalog_commit::commitRenameOfCollectionMetadata(
            opCtx,
            op.from,
            currentCatalog.at(op.from).uuid,
            op.to,
            existingDataTo.map([](const auto& pair) { return pair.first.getUuid(); }),
            boost::none,
            op.isUpgradingFCV,
            isCurrentPrimary);
    }

    void executeDDL(OperationContext* opCtx, const MovePrimaryDDL& op) {
        if (!((isCurrentPrimary && op.to != kThisShard) ||
              (!isCurrentPrimary && op.to == kThisShard))) {
            // Only valid states are moving primary outside of this shard or into this shard.
            return;
        }

        static const auto csReason = BSON("reason" << "movePrimary");
        {
            auto scopedCsr = DatabaseShardingRuntime::acquireExclusive(opCtx, kTargetNss.dbName());
            scopedCsr->enterCriticalSectionCatchUpPhase(opCtx, csReason);
            scopedCsr->enterCriticalSectionCommitPhase(opCtx, csReason);
        }
        ON_BLOCK_EXIT([&] {
            auto scopedCsr = DatabaseShardingRuntime::acquireExclusive(opCtx, kTargetNss.dbName());
            scopedCsr->exitCriticalSection(opCtx, csReason);
        });
        auto currDbEntry =
            actualFixture->mockCatalogClient()->updateAtomically([&](auto& client, auto pass) {
                auto currDbEntry = client.getDatabase(pass);
                currDbEntry.setPrimary(op.to);
                currDbEntry.setVersion(currDbEntry.getVersion().makeUpdated());
                client.setDatabaseEntry(pass, currDbEntry);
                return currDbEntry;
            });
        if (op.to == kThisShard) {
            for (const auto& [nss, collInfo] : currentCatalog) {
                if (collInfo.collType == ShardingCollType::kUntracked) {
                    // Untracked collections don't have any metadata to handle
                    continue;
                }
                shard_catalog_commit::commitChunklessCollectionMetadataLocally(opCtx, nss);
            }
            shard_catalog_commit::commitCreateDatabaseMetadataLocally(opCtx, currDbEntry);
        } else {
            shard_catalog_commit::commitDropDatabaseMetadataLocally(opCtx, kTargetNss.dbName());
        }

        isCurrentPrimary = (op.to == kThisShard);
    }

    void executeDDL(OperationContext* opCtx, const SplitChunkDDL& op) {
        // If the collection isn't sharded do an early return
        {
            const auto it = currentCatalog.find(op.nss);
            if (it == currentCatalog.end())
                return;
            if (it->second.collType != ShardingCollType::kSharded)
                return;
        }

        static const auto csReason = BSON("reason" << "splitChunk");

        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->enterCriticalSectionCatchUpPhase(opCtx, csReason);
            scopedCsr->enterCriticalSectionCommitPhase(opCtx, csReason);
        }
        ON_BLOCK_EXIT([&] {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->exitCriticalSection(opCtx, csReason);
        });

        const auto shardKey = BSON("_id" << op.key);
        auto hasSplitChunk = actualFixture->mockCatalogClient()->updateAtomically([&](auto& client,
                                                                                      auto pass) {
            auto [collEntry, origChunks] = client.getCollectionMetadata(pass, op.nss);
            const auto origChunkIt =
                std::find_if(origChunks.begin(), origChunks.end(), [&](const auto& chunk) {
                    return chunk.getRange().containsKey(shardKey);
                });
            auto newPlacementVersion =
                std::max_element(origChunks.begin(),
                                 origChunks.end(),
                                 [](const ChunkType& left, const auto& right) {
                                     return (left.getVersion() <=> right.getVersion()) ==
                                         std::partial_ordering::less;
                                 })
                    ->getVersion();
            auto originalRange = origChunkIt->getRange();

            if (SimpleBSONObjComparator::kInstance.evaluate(shardKey == originalRange.getMin()) ||
                SimpleBSONObjComparator::kInstance.evaluate(shardKey == originalRange.getMax())) {
                // The split point can't be one of the edges as that would result in an empty chunk
                // range.
                return false;
            }

            auto newChunkLeft = *origChunkIt;
            newChunkLeft.setRange({originalRange.getMin(), shardKey});
            newPlacementVersion.incMinor();
            newChunkLeft.setVersion(newPlacementVersion);
            newChunkLeft.setEstimatedSizeBytes(boost::none);
            newChunkLeft.setJumbo(false);

            auto newChunkRight = *origChunkIt;
            newChunkRight.setName(OID::gen());
            newChunkRight.setRange({shardKey, originalRange.getMax()});
            newPlacementVersion.incMinor();
            newChunkRight.setVersion(newPlacementVersion);
            newChunkRight.setEstimatedSizeBytes(boost::none);
            newChunkRight.setJumbo(false);

            origChunks.erase(origChunkIt);
            origChunks.emplace_back(std::move(newChunkLeft));
            origChunks.emplace_back(std::move(newChunkRight));
            client.setCollectionMetadata(pass, std::move(collEntry), std::move(origChunks));
            return true;
        });

        if (hasSplitChunk) {
            shard_catalog_commit::commitCollectionMetadataLocally(opCtx, op.nss, isCurrentPrimary);
        }
    }

    void executeDDL(OperationContext* opCtx, const MergeChunkDDL& op) {
        // If the collection isn't sharded do an early return
        {
            const auto it = currentCatalog.find(op.nss);
            if (it == currentCatalog.end())
                return;
            if (it->second.collType != ShardingCollType::kSharded)
                return;
        }

        static const auto csReason = BSON("reason" << "mergeChunk");

        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->enterCriticalSectionCatchUpPhase(opCtx, csReason);
            scopedCsr->enterCriticalSectionCommitPhase(opCtx, csReason);
        }
        ON_BLOCK_EXIT([&] {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->exitCriticalSection(opCtx, csReason);
        });

        const auto fromKey = BSON("_id" << op.from);
        const auto toKey = BSON("_id" << op.to);
        auto mergedRange = ChunkRange{fromKey, toKey};
        auto hasMergedChunks =
            actualFixture->mockCatalogClient()->updateAtomically([&](auto& client, auto pass) {
                auto [collEntry, origChunks] = client.getCollectionMetadata(pass, op.nss);

                auto newPlacementVersion =
                    std::max_element(origChunks.begin(),
                                     origChunks.end(),
                                     [](const ChunkType& left, const auto& right) {
                                         return (left.getVersion() <=> right.getVersion()) ==
                                             std::partial_ordering::less;
                                     })
                        ->getVersion();
                // Ensure that all the merged chunks are owned by the same shard and replace them
                // with a single new entry.
                auto overlappingItStart =
                    std::remove_if(origChunks.begin(), origChunks.end(), [&](const auto& chunk) {
                        return chunk.getRange().overlaps(mergedRange);
                    });
                const auto numChunksToMerge = std::distance(overlappingItStart, origChunks.end());
                if (numChunksToMerge == 1) {
                    // Only a single chunk would be merged, this is a no-op.
                    return false;
                }
                invariant(numChunksToMerge > 0);
                auto ownerShard = overlappingItStart->getShard();
                auto allChunksOwnedBySameShard =
                    std::all_of(overlappingItStart, origChunks.end(), [&](const auto& chunk) {
                        return chunk.getShard() == ownerShard;
                    });
                if (!allChunksOwnedBySameShard) {
                    return false;
                }


                auto mergedChunk = *overlappingItStart;
                mergedChunk.setRange(mergedRange);
                newPlacementVersion.incMinor();
                mergedChunk.setVersion(newPlacementVersion);
                mergedChunk.setEstimatedSizeBytes(boost::none);
                auto mergeTs = nextClusterTime();
                mergedChunk.setOnCurrentShardSince(mergeTs);
                mergedChunk.setHistory(
                    {ChunkHistory(*mergedChunk.getOnCurrentShardSince(), mergedChunk.getShard())});

                // Replace now all the chunks to be merged with the new chunk entry.
                origChunks.erase(overlappingItStart, origChunks.end());
                origChunks.emplace_back(std::move(mergedChunk));
                client.setCollectionMetadata(pass, std::move(collEntry), std::move(origChunks));
                return true;
            });

        if (hasMergedChunks) {
            shard_catalog_commit::commitCollectionMetadataLocally(opCtx, op.nss, isCurrentPrimary);
        }
    }

    void executeDDL(OperationContext* opCtx, const MoveChunkDDL& op) {
        // If the collection isn't sharded do an early return.
        {
            const auto it = currentCatalog.find(op.nss);
            if (it == currentCatalog.end())
                return;
            if (it->second.collType != ShardingCollType::kSharded)
                return;
        }

        static const auto csReason = BSON("reason" << "moveChunk");

        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->enterCriticalSectionCatchUpPhase(opCtx, csReason);
            scopedCsr->enterCriticalSectionCommitPhase(opCtx, csReason);
        }
        ON_BLOCK_EXIT([&] {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->exitCriticalSection(opCtx, csReason);
        });

        const auto shardKey = BSON("_id" << op.key);
        actualFixture->mockCatalogClient()->updateAtomically([&](auto& client, auto pass) {
            auto [collEntry, origChunks] = client.getCollectionMetadata(pass, op.nss);

            auto newPlacementVersion =
                std::max_element(origChunks.begin(),
                                 origChunks.end(),
                                 [](const ChunkType& left, const auto& right) {
                                     return (left.getVersion() <=> right.getVersion()) ==
                                         std::partial_ordering::less;
                                 })
                    ->getVersion();
            const auto origChunkIt =
                std::find_if(origChunks.begin(), origChunks.end(), [&](const auto& chunk) {
                    return chunk.getRange().containsKey(shardKey);
                });
            invariant(origChunkIt != origChunks.end());

            auto movedChunk = *origChunkIt;
            newPlacementVersion.incMajor();
            movedChunk.setVersion(newPlacementVersion);
            movedChunk.setEstimatedSizeBytes(boost::none);
            movedChunk.setJumbo(false);
            auto moveTs = nextClusterTime();
            movedChunk.setOnCurrentShardSince(moveTs);
            movedChunk.setShard(op.destination);

            // Add new entry and remove old placement version values to simulate the snapshot window
            // rolling off.
            auto newHistory = movedChunk.getHistory();
            newHistory.emplace(
                newHistory.begin(), *movedChunk.getOnCurrentShardSince(), movedChunk.getShard());
            const auto thresholdTs = Timestamp{
                static_cast<unsigned int>(std::max(1, static_cast<int>(moveTs.getSecs()) - 5)), 0};
            std::erase_if(newHistory,
                          [&](const auto& elem) { return elem.getValidAfter() < thresholdTs; });
            movedChunk.setHistory(std::move(newHistory));

            origChunks.erase(origChunkIt);
            origChunks.emplace_back(std::move(movedChunk));
            client.setCollectionMetadata(pass, std::move(collEntry), std::move(origChunks));
        });

        shard_catalog_commit::commitCollectionMetadataLocally(opCtx, op.nss, isCurrentPrimary);
    }

    void executeReshardingOp(OperationContext* opCtx,
                             const ReshardCollectionDDL& op,
                             bool isMoveCollection) {
        {
            const auto it = currentCatalog.find(op.nss);
            if (it == currentCatalog.end()) {
                return;
            }
            if (it->second.collType == ShardingCollType::kUntracked) {
                return;
            }
        }

        // Note, we deliberately reproduce the steps done by resharding with an intermediate
        // temporary collection in order to better reflect the actual code path used and to check
        // for its correctness.


        // 1. Create temporary collection with the new chunks
        const auto tempNss =
            NamespaceString::createNamespaceString_forTest("test.temporary_collection");
        const auto tempUuid = UUID::gen();
        ON_BLOCK_EXIT([&] {
            // Make sure the currentCatalog entry is updated since rename needs the proper UUID
            // used.
            currentCatalog.at(op.nss).uuid = tempUuid;
        });
        const auto targetUUID =
            actualFixture->mockCatalogClient()->getCollection(opCtx, op.nss, {}).getUuid();

        {
            const auto now = Date_t::now();
            Timestamp collTimestamp = nextClusterTime();

            CollectionType collEntry{
                tempNss, OID::gen(), collTimestamp, now, tempUuid, KeyPattern{BSON("_id" << 1)}};
            collEntry.setUnsplittable(isMoveCollection);

            std::vector<ChunkType> chunks;
            auto chunkVersion =
                ChunkVersion{CollectionGeneration{collEntry.getEpoch(), collEntry.getTimestamp()},
                             CollectionPlacement{1, 0}};
            for (const auto& chunk : op.newChunks) {
                auto& ct = chunks.emplace_back(
                    tempUuid, ChunkRange{chunk.min, chunk.max}, chunkVersion, chunk.owner);
                ct.setName(OID::gen());
                ct.setOnCurrentShardSince(collTimestamp);
                ct.setHistory(std::vector<ChunkHistory>{ChunkHistory{collTimestamp, chunk.owner}});
                chunkVersion.incMajor();
            }

            actualFixture->mockCatalogClient()->updateAtomically([&](auto& mockClient, auto pass) {
                mockClient.setCollectionMetadata(pass, std::move(collEntry), std::move(chunks));
            });

            shard_catalog_commit_for_resharding::commitCreateCollection(
                opCtx, tempNss, isCurrentPrimary);
        }


        // 2. Rename the temporary collection to the final target atomically
        static const auto csReason = BSON("reason" << "reshardCollection");

        {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->enterCriticalSectionCatchUpPhase(opCtx, csReason);
            scopedCsr->enterCriticalSectionCommitPhase(opCtx, csReason);
        }
        ON_BLOCK_EXIT([&] {
            auto scopedCsr = CollectionShardingRuntime::acquireExclusive(opCtx, op.nss);
            scopedCsr->exitCriticalSection(opCtx, csReason);
        });

        actualFixture->mockCatalogClient()->updateAtomically(
            [&](MockCatalogClient& mockClient, auto pass) {
                auto [collEntry, chunks] = mockClient.getCollectionMetadata(pass, tempNss);
                collEntry.setNss(op.nss);
                mockClient.setCollectionMetadata(pass, std::move(collEntry), std::move(chunks));
            });

        shard_catalog_commit_for_resharding::commitRenameOfTemporaryCollection(
            opCtx, tempNss, tempUuid, op.nss, targetUUID, op.isUpgrading, isCurrentPrimary);
    }

    void executeDDL(OperationContext* opCtx, const ReshardCollectionDDL& op) {
        return executeReshardingOp(opCtx, op, false);
    }

    void executeDDL(OperationContext* opCtx, const MoveCollectionDDL& op) {
        return executeReshardingOp(
            opCtx,
            ReshardCollectionDDL{op.nss,
                                 {Chunk{BSON("_id" << MINKEY), BSON("_id" << MAXKEY), op.to}},
                                 op.isUpgrading},
            true);
    }

    void executeDDL(OperationContext* opCtx, const ClearMetadataDDL& clear) {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, clear.nss);
        csr->clearCollectionMetadata(opCtx);
    }

    void checkInvariants(OperationContext* opCtx, const NamespaceString& nss) {
        // Invariants to check:
        // * Router-role acquired shard version and Shard-role acquired shard version are in
        //   agreement
        // * The router-role routing table for the collection can be checked for ownership in the
        //   shard and matches the router's view
        sharding::router::CollectionRouter routingCtx{opCtx, nss};
        routingCtx.route("router", [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
            boost::optional<ScopedSetShardRole> setShardRole;
            bool isTracked = cri.hasRoutingTable();
            if (isTracked) {
                auto shardVersion = cri.getShardVersion(opCtx, kThisShard);
                setShardRole.emplace(opCtx, nss, shardVersion, boost::none);
            } else {
                setShardRole.emplace(opCtx, nss, ShardVersion::UNTRACKED(), cri.getDbVersion());
            }

            shard_role_loop::withStaleShardRetry(opCtx, [&] {
                auto acq = acquireCollectionMaybeLockFree(
                    opCtx,
                    CollectionAcquisitionRequest::fromOpCtx(
                        opCtx, nss, AcquisitionPrerequisites::kRead));
                const auto& shardingDesc = acq.getShardingDescription();
                if (!isTracked) {
                    invariant(!shardingDesc.hasRoutingTable());
                    invariant(cri.getDbPrimaryShardRef() == kThisShard);
                    return;
                }

                cri.getChunkManager().forEachChunk([&](const auto& chunk) {
                    bool isOwnedByShard = chunk.getShardRef() == kThisShard;
                    bool isInHistory = std::any_of(chunk.getHistory().begin(),
                                                   chunk.getHistory().end(),
                                                   [&](const auto& historyElem) {
                                                       return historyElem.getShard() == kThisShard;
                                                   });
                    if (isOwnedByShard || isInHistory) {
                        if (!shardingDesc.isSharded()) {
                            // This is an unsplittable collection so there should be only a single
                            // chunk.
                            const auto expectedFullRange =
                                ChunkRange{BSON("_id" << MINKEY), BSON("_id" << MAXKEY)};
                            invariant(chunk.getRange() == expectedFullRange);
                            return false;
                        }

                        // If the router believes this shard should handle the chunk then the shard
                        // should be able to handle all queries against keys in that chunk.
                        std::vector<ChunkRange> shardRanges;
                        shardingDesc.forEachOverlappingChunk(
                            chunk.getMin(), chunk.getMax(), false, [&](const auto& shardChunk) {
                                if (isOwnedByShard) {
                                    invariant(shardChunk.getShardRef() == kThisShard);
                                } else {
                                    invariant(shardChunk.getShardRef() != kThisShard);
                                }
                                shardRanges.emplace_back(shardChunk.getRange());
                                return true;
                            });
                        std::sort(shardRanges.begin(), shardRanges.end());

                        invariant(!shardRanges.empty());
                        ChunkRange coveredRange = shardRanges.front();
                        ChunkRange rangeMissing = chunk.getRange();
                        for (const auto& shardRange : shardRanges) {
                            // The new shard chunk range must be an extension of the current running
                            // total chunk range to avoid having gaps in ranges.
                            invariant(coveredRange.overlaps(shardRange) ||
                                      coveredRange.getMax().woCompare(shardRange.getMin()) == 0);
                            coveredRange = coveredRange.unionWith(shardRange);
                        }
                        // The covered shard range must contain the target chunk range.
                        invariant(rangeMissing.overlapWith(coveredRange) == rangeMissing);
                    }
                    return true;
                });
            });
        });
    }

    void reset() {
        isCurrentPrimary = true;
        currentCatalog.clear();

        // Delete shard.catalog.* contents
        {
            DBDirectClient client{actualFixture->getFixtureOpCtx()};
            client.dropCollection(NamespaceString::kConfigShardCatalogCollectionsNamespace);
            client.dropCollection(NamespaceString::kConfigShardCatalogChunksNamespace);
        }

        // And reset the "CSRS" contents
        auto dbEntry =
            actualFixture->mockCatalogClient()->updateAtomically([&](auto& client, auto pass) {
                for (const auto& nss : kNamespacesToTest) {
                    client.eraseCollectionMetadata(pass, nss);
                    auto scopedCsr = CollectionShardingRuntime::acquireExclusive(
                        actualFixture->getFixtureOpCtx(), nss);
                    scopedCsr->clearCollectionMetadata(actualFixture->getFixtureOpCtx(), true);
                }

                auto currDbEntry = client.getDatabase(pass);
                currDbEntry.setPrimary(kThisShard);
                currDbEntry.setVersion(currDbEntry.getVersion().makeUpdated());
                client.setDatabaseEntry(pass, currDbEntry);
                return currDbEntry;
            });

        // The shard is now back to being the primary.
        shard_catalog_commit::commitCreateDatabaseMetadataLocally(actualFixture->getFixtureOpCtx(),
                                                                  dbEntry);
    };

    void executeDDLOperations(const std::vector<DDL>& ddls, std::atomic_flag& doneSignal) {
        ON_BLOCK_EXIT([&] { doneSignal.test_and_set(); });
        ThreadClient client{"BackgroundDDLExecutor", actualFixture->getService()};
        for (const auto& ddl : ddls) {
            auto opCtx = client->makeOperationContext();
            executeOperation(opCtx.get(), ddl);
        }
    }

    bool isCurrentPrimary = true;
    struct CollInfo {
        UUID uuid;
        ShardingCollType collType;
    };
    stdx::unordered_map<NamespaceString, CollInfo> currentCatalog;
    boost::optional<ShardServerFixture> actualFixture;
    std::vector<unittest::ServerParameterGuard> featureFlagGuards;
    Timestamp currentClusterTime{1, 0};

public:
    DDLStateMachine() {
        // TODO SERVER-129585: Get rid of the initializer function call once fuzztest binaries
        // initialize things properly.
        runGlobalInitializersOrDie({});

        featureFlagGuards.emplace_back("featureFlagAuthoritativeShardsDDL", true);
        featureFlagGuards.emplace_back("featureFlagAuthoritativeShardsCRUD", true);

        actualFixture.emplace();
        actualFixture->SetUp();

        actualFixture->mockCatalogClient()->updateAtomically([&](auto& client, auto pass) {
            DatabaseType dbEntry{
                kTargetNss.dbName(), kThisShard, DatabaseVersion{UUID::gen(), Timestamp{1, 0}}};
            client.setDatabaseEntry(pass, std::move(dbEntry));
        });
    }

    ~DDLStateMachine() {
        actualFixture->TearDown();
    }

    void FuzzDDLInvariants(const std::vector<DDL>& ddls) {
        reset();
        std::atomic_flag doneSignal;
        auto backgroundWriter = stdx::thread(
            &DDLStateMachine::executeDDLOperations, this, std::ref(ddls), std::ref(doneSignal));

        auto service = actualFixture->getService();
        ASSERT_NE(service, nullptr);
        auto client = service->makeClient("ReaderThread");
        ASSERT_NE(client.get(), nullptr);
        AlternativeClientRegion acr{client};

        while (!doneSignal.test()) {
            for (const auto& nss : kNamespacesToTest) {
                auto opCtx = acr->makeOperationContext();
                checkInvariants(opCtx.get(), nss);
            }
            std::this_thread::yield();
        }

        backgroundWriter.join();
    }
};

auto ArbitraryDDL() {
    return VariantOf(ClearMetadataDDL::arbitrary(),
                     CreateDDL::arbitrary(),
                     DropDDL::arbitrary(),
                     RenameDDL::arbitrary(),
                     MovePrimaryDDL::arbitrary(),
                     SplitChunkDDL::arbitrary(),
                     MergeChunkDDL::arbitrary(),
                     MoveChunkDDL::arbitrary(),
                     ReshardCollectionDDL::arbitrary(),
                     MoveCollectionDDL::arbitrary());
}

FUZZ_TEST_F(DDLStateMachine, FuzzDDLInvariants)
    .WithDomains(VectorOf(ArbitraryDDL()).WithMaxSize(256));

}  // namespace
}  // namespace mongo
