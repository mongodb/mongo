/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/base/initializer.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/scopeguard.h"

#include <atomic>
#include <vector>

#include "fuzztest/fuzztest.h"

namespace mongo {
namespace {

using namespace fuzztest;

const NamespaceString kTargetNss = NamespaceString::createNamespaceString_forTest("test.target");
const NamespaceString kOtherColl1Nss =
    NamespaceString::createNamespaceString_forTest("test.other1");
const NamespaceString kOtherColl2Nss =
    NamespaceString::createNamespaceString_forTest("test.other2");

auto ArbitraryNamespace() {
    return ElementOf<NamespaceString>({kTargetNss, kOtherColl1Nss, kOtherColl2Nss});
}

const ShardRef kThisShard{"myShardName"};
const ShardRef kOtherShard1{"other1"};
const ShardRef kOtherShard2{"other2"};

auto ArbitraryShardRef() {
    return ElementOf<ShardRef>({kThisShard, kOtherShard1, kOtherShard2});
}

struct ClearMetadataDDL {};

enum class ShardingCollType { kUntracked, kUnsplittable, kSharded };

struct CreateDDL {
    struct Chunk {
        BSONObj min;
        BSONObj max;
        ShardRef owner;
    };

    NamespaceString nss;
    ShardingCollType shardingType;
    std::vector<Chunk> chunks;

    static auto arbitrary() {
        auto arbCollectionType = ElementOf<ShardingCollType>({ShardingCollType::kUntracked,
                                                              ShardingCollType::kUnsplittable,
                                                              ShardingCollType::kSharded});
        auto arbTargetCollection = ArbitraryNamespace();

        auto arbSplitPointsWithOwners = FlatMap(
            [](auto splitPoints) {
                std::sort(splitPoints.begin(), splitPoints.end());
                return PairOf(Just(splitPoints),
                              VectorOf(ArbitraryShardRef()).WithSize(splitPoints.size()));
            },
            NonEmpty(UniqueElementsVectorOf(Arbitrary<int>()).WithMaxSize(16)));
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
        auto arbUnsplittableChunk = Map(
            [](const auto& shardRef) {
                return Chunk{BSON("_id" << MINKEY), BSON("_id" << MAXKEY), shardRef};
            },
            ArbitraryShardRef());
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
                            Just(nss), Just(collType), Just(std::vector<Chunk>{unsplittableChunk}));
                    case ShardingCollType::kSharded:
                        return StructOf<CreateDDL>(
                            Just(nss), Just(collType), Just(std::move(chunks)));
                }
            },
            arbCollectionType,
            arbTargetCollection,
            arbChunks,
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

using DDL = std::variant<ClearMetadataDDL, CreateDDL, DropDDL, RenameDDL>;

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

    template <typename F>
    void updateAtomically(F&& f) {
        std::unique_lock lk{_mutex};
        f(*this, UpdatePass{});
    }

private:
    std::vector<std::pair<CollectionType, std::vector<ChunkType>>> _collsAndChunks;
    std::mutex _mutex;
};

class ShardServerFixture : public ShardServerTestFixture {
public:
    MockCatalogClient* mockCatalogClient() {
        return _mockCatalogClient;
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

        static Timestamp nextCollTimestamp = Timestamp(1, 0);
        const auto now = Date_t::now();
        Timestamp collTimestamp = nextCollTimestamp;
        nextCollTimestamp = Timestamp(collTimestamp.getSecs() + 1, collTimestamp.getInc());

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

    void executeDDL(OperationContext* opCtx, const ClearMetadataDDL& clear) {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTargetNss);
        csr->clearCollectionMetadata(opCtx);
    }

    void checkInvariants() {
        // TODO SERVER-129227: Implement this
    }

    void reset() {
        isCurrentPrimary = true;
        currentCatalog.clear();
    };

    void executeDDLOperations(const std::vector<DDL>& ddls, std::atomic_flag& doneSignal) {
        ON_BLOCK_EXIT([&] { doneSignal.test_and_set(); });
        ThreadClient client{"BackgroundDDLExecutor",
                            actualFixture->getServiceContext()->getService()};
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

public:
    DDLStateMachine() {
        // TODO SERVER-129585: Get rid of the initializer function call once fuzztest binaries
        // initialize things properly.
        runGlobalInitializersOrDie({});
        actualFixture.emplace();
        actualFixture->SetUp();
    }

    ~DDLStateMachine() {
        actualFixture->TearDown();
    }

    void FuzzDDLInvariants(const std::vector<DDL>& ddls) {
        reset();
        std::atomic_flag doneSignal;
        auto backgroundWriter = stdx::thread(
            &DDLStateMachine::executeDDLOperations, this, std::ref(ddls), std::ref(doneSignal));

        while (!doneSignal.test()) {
            checkInvariants();
            std::this_thread::yield();
        }

        backgroundWriter.join();
    }
};

auto ArbitraryDDL() {
    return VariantOf(Just(ClearMetadataDDL()),
                     CreateDDL::arbitrary(),
                     DropDDL::arbitrary(),
                     RenameDDL::arbitrary());
}

FUZZ_TEST_F(DDLStateMachine, FuzzDDLInvariants)
    .WithDomains(VectorOf(ArbitraryDDL()).WithMaxSize(256));

}  // namespace
}  // namespace mongo
