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

#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"

#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/shard_role/shard_catalog/collection_cache_recoverer.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/type_oplog_catalog_metadata_gen.h"
#include "mongo/db/sharding_environment/shard_server_op_observer.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const NamespaceString kFromNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "FromColl");
const NamespaceString kToNss = NamespaceString::createNamespaceString_forTest("TestDB", "ToColl");
const std::string kShardKey = "_id";

// Builds a point on the shard key, e.g. key(50) -> {_id: 50}. Accepts MINKEY/MAXKEY too.
BSONObj key(const auto& value) {
    return BSON(kShardKey << value);
}

const BSONObj kShardKeyPattern = key(1);

class MockCatalogClient final : public ShardingCatalogClientMock {
public:
    using ShardingCatalogClientMock::getCollection;

    void setCollectionMetadata(CollectionType coll, std::vector<ChunkType> chunks) {
        _notFound = false;
        _coll = std::move(coll);
        _chunks = std::move(chunks);
    }

    void setCollectionNotFound() {
        _notFound = true;
    }

    CollectionType getCollection(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 repl::ReadConcernArgs readConcern) override {
        uassert(ErrorCodes::NamespaceNotFound, "Collection not found in mock", !_notFound);
        return _coll;
    }

    StatusWith<std::vector<ChunkType>> getChunks(
        OperationContext* opCtx,
        const BSONObj& filter,
        const BSONObj& sort,
        boost::optional<int> limit,
        repl::OpTime* opTime,
        const OID& epoch,
        const Timestamp& timestamp,
        repl::ReadConcernArgs readConcern,
        const boost::optional<BSONObj>& hint = boost::none) override {
        return _chunks;
    }

    repl::OpTimeWith<std::vector<ShardType>> getAllShards(OperationContext* opCtx,
                                                          repl::ReadConcernArgs readConcern,
                                                          BSONObj filter = BSONObj()) override {
        return repl::OpTimeWith<std::vector<ShardType>>(std::vector<ShardType>{});
    }

private:
    CollectionType _coll;
    std::vector<ChunkType> _chunks;
    bool _notFound = false;
};

class ShardCatalogWriteOrderObserver final : public OpObserverNoop {
public:
    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) override {
        _recordShardCatalogWrite(coll->ns());
    }

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override {
        _recordShardCatalogWrite(args.coll->ns());
    }

    const std::vector<NamespaceString>& writes() const {
        return _writes;
    }

private:
    void _recordShardCatalogWrite(const NamespaceString& nss) {
        if (nss == NamespaceString::kConfigShardCatalogChunksNamespace ||
            nss == NamespaceString::kConfigShardCatalogCollectionsNamespace) {
            _writes.push_back(nss);
        }
    }

    std::vector<NamespaceString> _writes;
};

struct CollectionAndChunksMetadata {
    CollectionType collType;
    std::vector<ChunkType> chunks;
};

CollectionAndChunksMetadata makeCollectionMetadata(const NamespaceString& nss, int nChunks) {
    const UUID uuid = UUID::gen();
    const OID epoch = OID::gen();
    const Timestamp timestamp(Date_t::now());

    CollectionType collType{nss, epoch, timestamp, Date_t::now(), uuid, kShardKeyPattern};

    std::vector<ChunkType> chunks;
    auto chunkVersion = ChunkVersion({epoch, timestamp}, {1, 0});
    for (int i = 0; i < nChunks; i++) {
        auto min = i == 0 ? key(MINKEY) : key((i * 100));
        auto max = i == (nChunks - 1) ? key(MAXKEY) : key(((i + 1) * 100));
        auto range = ChunkRange(min, max);
        auto& chunk = chunks.emplace_back(uuid, std::move(range), chunkVersion, ShardId("0"));
        chunk.setName(OID::gen());
        chunkVersion.incMajor();
    }

    return {std::move(collType), std::move(chunks)};
}

CollectionAndChunksMetadata makeCollectionMetadata(int nChunks) {
    return makeCollectionMetadata(kTestNss, nChunks);
}

ChunkType makeChunk(const CollectionType& collType,
                    BSONObj min,
                    BSONObj max,
                    ChunkVersion version,
                    ShardId shardId = ShardId("0")) {
    ChunkType chunk(collType.getUuid(),
                    ChunkRange(std::move(min), std::move(max)),
                    version,
                    std::move(shardId));
    chunk.setName(OID::gen());
    return chunk;
}

std::vector<BSONObj> toConfigBSONVector(const std::vector<ChunkType>& chunks) {
    std::vector<BSONObj> docs;
    docs.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        docs.push_back(chunk.toConfigBSON());
    }
    return docs;
}

std::vector<ChunkType> makeSplitChunks(const CollectionType& collType, const ChunkType& chunk) {
    auto splitVersion = chunk.getVersion();
    splitVersion.incMajor();
    auto splitFirst = makeChunk(collType, chunk.getMin(), key(50), splitVersion);

    splitVersion.incMinor();
    auto splitSecond = makeChunk(collType, key(50), chunk.getMax(), splitVersion);

    return {std::move(splitFirst), std::move(splitSecond)};
}

void addHistoryEntries(ChunkType& chunk, int numHistoryEntries) {
    std::vector<ChunkHistory> history;
    history.reserve(numHistoryEntries);
    for (int i = 0; i < numHistoryEntries; ++i) {
        history.emplace_back(Timestamp(numHistoryEntries - i, 1), chunk.getShard());
    }

    chunk.setOnCurrentShardSince(history.front().getValidAfter());
    chunk.setHistory(std::move(history));
}

std::vector<ChunkType> makeCoveringChunks(const CollectionType& collType,
                                          int nChunks,
                                          ChunkVersion chunkVersion,
                                          int numHistoryEntries = 0) {
    std::vector<ChunkType> chunks;
    chunks.reserve(nChunks);
    for (int i = 0; i < nChunks; ++i) {
        chunkVersion.incMajor();
        auto min = i == 0 ? key(MINKEY) : key(i);
        auto max = i == (nChunks - 1) ? key(MAXKEY) : key((i + 1));
        auto chunk = makeChunk(collType, min, max, chunkVersion);
        if (numHistoryEntries > 0) {
            addHistoryEntries(chunk, numHistoryEntries);
        }
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

class CommitCollectionMetadataLocallyTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
        createTestCollection(operationContext(),
                             NamespaceString::kConfigShardCatalogCollectionsNamespace);
        createTestCollection(operationContext(),
                             NamespaceString::kConfigShardCatalogChunksNamespace);
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        auto client = std::make_unique<MockCatalogClient>();
        _mockCatalogClient = client.get();
        return client;
    }

    MockCatalogClient* mockCatalogClient() {
        return _mockCatalogClient;
    }

    long long countLocalDocs(const NamespaceString& nss) {
        DBDirectClient client(operationContext());
        return client.count(nss);
    }

    std::vector<BSONObj> findLocalDocs(const NamespaceString& nss, BSONObj query = BSONObj()) {
        DBDirectClient client(operationContext());
        FindCommandRequest findCmd(nss);
        findCmd.setFilter(std::move(query));
        auto cursor = client.find(std::move(findCmd));
        std::vector<BSONObj> results;
        while (cursor->more()) {
            results.push_back(cursor->next().getOwned());
        }
        return results;
    }

    void seedShardCatalog(const CollectionType& coll, const std::vector<ChunkType>& chunks) {
        DBDirectClient client(operationContext());
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                      coll.asShardCatalogType().toBSON());
        for (const auto& chunk : chunks) {
            client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                          chunk.toConfigBSON());
        }
    }

    // Installs an in-memory CSR whose routing table holds ALL of the collection's chunks and does
    // not allow gaps: a legacy, non-authoritative full routing table (as built by the router-style
    // catalog cache). This is the shape a shard's CSR can be left in after an FCV downgrade, before
    // the authoritative-catalog upgrade migrates it.
    void installLegacyFullTableCsr(const CollectionType& collType,
                                   const std::vector<ChunkType>& chunks) {
        auto rt = RoutingTableHistory::makeNew(kTestNss,
                                               collType.getUuid(),
                                               collType.getKeyPattern(),
                                               false /* unsplittable */,
                                               nullptr /* defaultCollator */,
                                               collType.getUnique(),
                                               collType.getEpoch(),
                                               collType.getTimestamp(),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true /* allowMigrations */,
                                               chunks);
        auto version = rt.getVersion();
        auto rtHandle = RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
        CollectionMetadata metadata(CurrentChunkManager(std::move(rtHandle)), kMyShardName);

        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss);
        scopedCsr->setCollectionMetadata(operationContext(), std::move(metadata));
    }

    BSONObj findLastOplogEntry() {
        DBDirectClient client(operationContext());
        FindCommandRequest findCmd(NamespaceString::kRsOplogNamespace);
        findCmd.setSort(BSON("$natural" << -1));
        findCmd.setLimit(1);
        auto cursor = client.find(std::move(findCmd));
        ASSERT_TRUE(cursor->more());
        return cursor->next().getOwned();
    }

    ShardCatalogWriteOrderObserver* installWriteOrderObserver() {
        auto opObserverRegistry =
            checked_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        auto observer = std::make_unique<ShardCatalogWriteOrderObserver>();
        auto observerPtr = observer.get();
        opObserverRegistry->addObserver(std::move(observer));
        return observerPtr;
    }

    long long countCommandOplogEntries(std::string_view commandField, const NamespaceString& nss) {
        const std::string objField = "o." + std::string{commandField};
        return findLocalDocs(NamespaceString::kRsOplogNamespace,
                             BSON("op" << "c" << objField << nss.coll()))
            .size();
    }

    repl::OplogEntry makeInvalidateOplogEntry(const UUID& uuid,
                                              const InvalidateCollectionMetadataOplogEntry& entry) {
        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kCommand);
        oplogEntry.setNss(kTestNss.getCommandNS());
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(entry.toBSON());
        oplogEntry.setOpTime(OplogSlot());
        oplogEntry.setWallClockTime(Date_t::now());
        return repl::OplogEntry(oplogEntry.toBSON());
    }

    BSONObj getRecoveryStats() {
        BSONObjBuilder builder;
        ShardingStatistics::get(operationContext()).report(&builder);
        return builder.obj().getObjectField("collectionShardingMetadataStatistics").getOwned();
    }

    BSONObj getChunkOpStats() {
        BSONObjBuilder builder;
        ShardingStatistics::get(operationContext()).report(&builder);
        return builder.obj().getObjectField("chunkOperationsStatistics").getOwned();
    }

private:
    MockCatalogClient* _mockCatalogClient = nullptr;
};

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyPersistsCollectionAndChunks) {
    auto [collType, chunks] = makeCollectionMetadata(3);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), collType.getUuid());

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    ASSERT_EQ(chunkDocs.size(), 3u);
}

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyUpdatesCSR) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->isSharded());
    ASSERT_EQ(metadata->getChunkManager()->getUUID(), collType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyIsIdempotent) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionPersistsCollectionAndChunks) {
    auto [collType, chunks] = makeCollectionMetadata(3);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    // The commit is counted distinctly from the invalidate it emits, and is not a clone.
    ASSERT_EQ(getRecoveryStats().getIntField("countLocalCollectionMetadataCommits"), 1);
    ASSERT_EQ(getRecoveryStats().getIntField("countLocalCollectionMetadataClones"), 0);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), collType.getUuid());

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    ASSERT_EQ(chunkDocs.size(), 3u);
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionUpdatesCSR) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->isSharded());
    ASSERT_EQ(metadata->getChunkManager()->getUUID(), collType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionIsIdempotent) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionReplacesStaleChunksOnReissuedOIDs) {
    // First pass: persist the initial chunks for the collection.
    auto [collType, chunksPass1] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunksPass1);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    // Second pass: same UUID and ranges, but with freshly generated chunk OIDs (mimicking the
    // unsplittable->sharded transition where the global catalog reissues chunk OIDs).
    auto chunksPass2 = chunksPass1;
    for (auto& chunk : chunksPass2) {
        chunk.setName(OID::gen());
    }
    mockCatalogClient()->setCollectionMetadata(collType, chunksPass2);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    // Only the second-pass chunks should remain; the first-pass rows must be deleted, not appended.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    std::set<OID> persistedNames;
    for (const auto& doc : chunkDocs) {
        persistedNames.insert(doc.getField(ChunkType::name.name()).OID());
    }
    for (const auto& chunk : chunksPass2) {
        ASSERT(persistedNames.count(chunk.getName()))
            << "expected new-OID chunk " << chunk.getName() << " to be persisted";
    }
    for (const auto& chunk : chunksPass1) {
        ASSERT(!persistedNames.count(chunk.getName()))
            << "stale chunk " << chunk.getName() << " should have been deleted";
    }
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsWritesDeltaOplogEntryForSmallPayload) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    auto splitChunks = makeSplitChunks(collType, chunks[0]);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(splitChunks));

    auto oplogEntry = findLastOplogEntry();
    auto object = oplogEntry.getObjectField("o");
    ASSERT_EQ(object.firstElementFieldNameStringData(), "updateCollectionMetadata");
    ASSERT_EQ(object.firstElement().valueStringData(), kTestNss.coll());
    ASSERT_EQ(object.getField("changedChunks").Obj().nFields(), 2);
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsWritesDeltaOplogEntryAtChunkLimit) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    constexpr int kNumChangedChunks = 100;
    auto changedChunks =
        makeCoveringChunks(collType, kNumChangedChunks, chunks.back().getVersion());

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(changedChunks));

    auto oplogEntry = findLastOplogEntry();
    auto object = oplogEntry.getObjectField("o");
    ASSERT_EQ(object.firstElementFieldNameStringData(), "updateCollectionMetadata");
    ASSERT_EQ(object.getField("changedChunks").Obj().nFields(), kNumChangedChunks);
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsFallsBackToInvalidateOverChunkLimit) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    auto changedChunks = makeCoveringChunks(collType, 101, chunks.back().getVersion());

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(changedChunks));

    auto oplogEntry = findLastOplogEntry();
    auto object = oplogEntry.getObjectField("o");
    ASSERT_EQ(object.firstElementFieldNameStringData(), "invalidateCollectionMetadata");
    ASSERT_FALSE(object.hasField("changedChunks"));
}

TEST_F(CommitCollectionMetadataLocallyTest,
       ChunkOperationsFallsBackToInvalidateForOversizedPayload) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    auto changedChunks = makeCoveringChunks(
        collType, 40 /* nChunks */, chunks.back().getVersion(), 12000 /* numHistoryEntries */);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(changedChunks));

    auto oplogEntry = findLastOplogEntry();
    auto object = oplogEntry.getObjectField("o");
    ASSERT_EQ(object.firstElementFieldNameStringData(), "invalidateCollectionMetadata");
    ASSERT_FALSE(object.hasField("changedChunks"));
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsPersistsSplitToDisk) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    auto splitChunks = makeSplitChunks(collType, chunks[0]);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(splitChunks));

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    std::set<OID> persistedNames;
    for (const auto& doc : chunkDocs) {
        persistedNames.insert(doc.getField(ChunkType::name.name()).OID());
    }
    ASSERT_EQ(persistedNames.count(splitChunks[0].getName()), 1);
    ASSERT_EQ(persistedNames.count(splitChunks[1].getName()), 1);
    ASSERT_EQ(persistedNames.count(chunks[0].getName()), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsRecordsShardCatalogCommitStatistics) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    auto splitChunks = makeSplitChunks(collType, chunks[0]);
    ASSERT_EQ(splitChunks.size(), 2u);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(splitChunks));

    // Each commit counts once; the chunk total accumulates the number of chunks written.
    auto stats = getChunkOpStats();
    ASSERT_EQ(stats.getIntField("countLocalChunkOperationsMetadataCommits"), 1);
    ASSERT_EQ(stats.getIntField("countChunksCommittedToShardCatalog"), 2);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(splitChunks));

    stats = getChunkOpStats();
    ASSERT_EQ(stats.getIntField("countLocalChunkOperationsMetadataCommits"), 2);
    ASSERT_EQ(stats.getIntField("countChunksCommittedToShardCatalog"), 4);
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsIsIdempotent) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    auto splitChunks = makeSplitChunks(collType, chunks[0]);
    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(splitChunks));
    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(splitChunks));

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    std::set<OID> persistedNames;
    for (const auto& doc : chunkDocs) {
        persistedNames.insert(doc.getField(ChunkType::name.name()).OID());
    }
    ASSERT_EQ(chunkDocs.size(), 2u);
    ASSERT_EQ(persistedNames.count(splitChunks[0].getName()), 1);
    ASSERT_EQ(persistedNames.count(splitChunks[1].getName()), 1);
    ASSERT_EQ(persistedNames.count(chunks[0].getName()), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsUpdatesCSR) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    auto splitChunks = makeSplitChunks(collType, chunks[0]);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(splitChunks));

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->isSharded());
    ASSERT_EQ(metadata->getChunkManager()->getUUID(), collType.getUuid());
    ASSERT_EQ(metadata->getChunkManager()->numChunks(), 2);
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsPreservesGapsInOwnedChunks) {
    // The shard owns a gapped set of chunks ([0, 10) and [30, 40)): it does not own the whole key
    // space, so its filtering metadata is built with real gaps (makeNewAllowingGaps). The chunks
    // use this node's shard id so keyBelongsToMe reflects real ownership.
    auto shardId = kMyShardName;
    auto [collType, _] = makeCollectionMetadata(0);
    auto ownedVersion = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {1, 0});
    auto owned1 = makeChunk(collType, key(0), key(10), ownedVersion, shardId);
    ownedVersion.incMajor();
    auto owned2 = makeChunk(collType, key(30), key(40), ownedVersion, shardId);
    mockCatalogClient()->setCollectionMetadata(collType, {owned1, owned2});

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        ASSERT_EQ(metadata->getChunkManager()->numChunks(), 2);
    }

    // Receiving a new chunk [50, 60) that is not adjacent to any owned chunk must not be rejected
    // as a gap, and the pre-existing gaps must be preserved.
    auto newVersion = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {3, 0});
    auto newChunk = makeChunk(collType, key(50), key(60), newVersion, shardId);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector({newChunk}));

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_EQ(metadata->getChunkManager()->numChunks(), 3);
    ASSERT(metadata->keyBelongsToMe(key(5)));
    ASSERT(metadata->keyBelongsToMe(key(35)));
    ASSERT(metadata->keyBelongsToMe(key(55)));
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsAppliesDeltaOntoLegacyFullTableCsr) {
    // The collection has a single chunk covering the whole key space.
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    // The shard's in-memory CSR is a legacy full routing table (does not allow gaps), the shape it
    // can be left in after an FCV downgrade because the authoritative-catalog upgrade does not
    // touch the CSR. The commit below is not marked as receiving a first chunk, so it takes the
    // incremental delta path and must merge onto this legacy base rather than reject it.
    installLegacyFullTableCsr(collType, chunks);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown()->allowGaps());
    }

    // A chunk operation delivers a single chunk covering only the upper part of the key space (e.g.
    // a moveRange recipient receiving [50, MaxKey)). It overlaps the base's [MinKey, MaxKey) chunk.
    // Applying it as an incremental delta on the legacy full table directly would discard
    // [MinKey, MaxKey) and leave a routing table whose first chunk no longer starts at MinKey,
    // which a non-gapped table rejects with ChunkMetadataInconsistency. The commit must instead
    // convert the base to allow gaps and then merge the delta, so this call must not throw.
    auto deltaVersion = chunks.back().getVersion();
    deltaVersion.incMajor();
    auto deltaChunk = makeChunk(collType, key(50), key(MAXKEY), deltaVersion, kMyShardName);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector({deltaChunk}));

    // The delta was merged onto the (converted) gap-allowing base, leaving this shard owning the
    // received chunk.
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->hasRoutingTable());
    ASSERT_TRUE(metadata->allowGaps());
    ASSERT_EQ(metadata->getChunkManager()->numChunks(), 1);
    ASSERT(metadata->keyBelongsToMe(key(55)));
}

// A shard receiving its FIRST chunk (as flagged by the caller) must not merge a delta onto whatever
// its CSR happens to hold: that base may be stale (here a legacy full routing table left by an FCV
// downgrade). Instead it bootstraps the collection metadata from the global catalog, so the CSR
// reflects the authoritative owned set rather than the stale full table.
TEST_F(CommitCollectionMetadataLocallyTest,
       ChunkOperationsReceivingFirstChunkBootstrapsFromGlobalCatalog) {
    // The authoritative global catalog has a single chunk covering the whole key space, at a
    // placement version newer than the stale CSR installed below -- as it always is in practice,
    // since the migration that hands over the first chunk bumps the collection placement version.
    auto [collType, _] = makeCollectionMetadata(1);
    auto authoritativeChunk =
        makeChunk(collType,
                  key(MINKEY),
                  key(MAXKEY),
                  ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {10, 0}),
                  kMyShardName);
    mockCatalogClient()->setCollectionMetadata(collType, {authoritativeChunk});

    // The in-memory CSR is a stale legacy full routing table with a different, two-chunk view of
    // the same collection at an older version (does not allow gaps).
    auto staleChunks = makeCoveringChunks(
        collType, 2, ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {1, 0}));
    installLegacyFullTableCsr(collType, staleChunks);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_EQ(metadata->getChunkManager()->numChunks(), 2);
        ASSERT_FALSE(metadata->allowGaps());
    }

    // Commit while flagged as receiving the first chunk. The passed chunks are ignored; the
    // metadata is bootstrapped from the global catalog instead of merged as a delta.
    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(),
        kTestNss,
        toConfigBSONVector({authoritativeChunk}),
        true /* receivingFirstChunk */);

    // The bootstrap clears the CSR via an invalidate rather than replicating a delta.
    ASSERT_EQ(countCommandOplogEntries("updateCollectionMetadata", kTestNss), 0);
    ASSERT_GTE(countCommandOplogEntries("invalidateCollectionMetadata", kTestNss), 1);

    // Receiving the first chunk is recorded on this (recipient) shard.
    ASSERT_EQ(getChunkOpStats().getIntField("countMoveRangeFirstChunkReceived"), 1);

    // The installed metadata reflects the global catalog (one chunk, gap-allowing), NOT the stale
    // two-chunk legacy CSR.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 1);
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->hasRoutingTable());
    ASSERT_TRUE(metadata->allowGaps());
    ASSERT_EQ(metadata->getChunkManager()->numChunks(), 1);
}

// Bootstrapping on a first-chunk commit is idempotent: a retry produces the same durable and
// in-memory state.
TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsReceivingFirstChunkIsIdempotent) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    auto chunkDocs = toConfigBSONVector(chunks);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, chunkDocs, true /* receivingFirstChunk */);
    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, chunkDocs, true /* receivingFirstChunk */);

    ASSERT_EQ(countCommandOplogEntries("updateCollectionMetadata", kTestNss), 0);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 1);
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->hasRoutingTable());
    ASSERT_EQ(metadata->getChunkManager()->numChunks(), 1);
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsMergeDeletesAllCoveredChunks) {
    // Start with the key space tiled as [MIN,1) [1,2) [2,3) [3,MAX).
    auto [collType, _] = makeCollectionMetadata(0);
    auto chunks = makeCoveringChunks(
        collType, 4, ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {1, 0}));
    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 4);

    // Merge the two middle chunks [1,2) and [2,3) into one [1,3). Both go away. The neighbours
    // [MIN,1) and [3,MAX) only touch the merged range at a boundary, so they stay.
    auto mergeVersion = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {5, 0});
    auto merged = makeChunk(collType, key(1), key(3), mergeVersion);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector({merged}));

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    std::set<OID> persistedNames;
    for (const auto& doc : chunkDocs) {
        persistedNames.insert(doc.getField(ChunkType::name.name()).OID());
    }
    ASSERT_EQ(chunkDocs.size(), 3u);
    ASSERT(persistedNames.count(chunks[0].getName()));   // [MIN,1) kept (only touches at boundary).
    ASSERT(persistedNames.count(merged.getName()));      // [1,3) inserted.
    ASSERT(persistedNames.count(chunks[3].getName()));   // [3,MAX) kept.
    ASSERT(!persistedNames.count(chunks[1].getName()));  // [1,2) deleted.
    ASSERT(!persistedNames.count(chunks[2].getName()));  // [2,3) deleted.
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsHandlesNonContiguousNewChunks) {
    // Tile the key space as [MIN,10) [10,20) [20,30) [30,MAX).
    auto [collType, _] = makeCollectionMetadata(0);
    auto chunkVersion = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {1, 0});
    std::vector<ChunkType> chunks;
    chunks.push_back(makeChunk(collType, key(MINKEY), key(10), chunkVersion));
    chunkVersion.incMajor();
    chunks.push_back(makeChunk(collType, key(10), key(20), chunkVersion));
    chunkVersion.incMajor();
    chunks.push_back(makeChunk(collType, key(20), key(30), chunkVersion));
    chunkVersion.incMajor();
    chunks.push_back(makeChunk(collType, key(30), key(MAXKEY), chunkVersion));
    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    // Split two chunks that are not next to each other in one operation: [MIN,10) into
    // [MIN,5),[5,10) and [20,30) into [20,25),[25,30). This gives two separate ranges. Each range
    // deletes only its own parent and leaves the other chunks alone.
    auto version = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {5, 0});
    auto firstA = makeChunk(collType, key(MINKEY), key(5), version);
    version.incMinor();
    auto firstB = makeChunk(collType, key(5), key(10), version);
    version.incMajor();
    auto thirdA = makeChunk(collType, key(20), key(25), version);
    version.incMinor();
    auto thirdB = makeChunk(collType, key(25), key(30), version);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector({firstA, firstB, thirdA, thirdB}));

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    std::set<OID> persistedNames;
    for (const auto& doc : chunkDocs) {
        persistedNames.insert(doc.getField(ChunkType::name.name()).OID());
    }
    ASSERT_EQ(chunkDocs.size(), 6u);
    ASSERT(!persistedNames.count(chunks[0].getName()));  // [MIN,10) split away.
    ASSERT(!persistedNames.count(chunks[2].getName()));  // [20,30) split away.
    ASSERT(persistedNames.count(chunks[1].getName()));   // [10,20) untouched.
    ASSERT(persistedNames.count(chunks[3].getName()));   // [30,MAX) untouched.
    ASSERT(persistedNames.count(firstA.getName()));
    ASSERT(persistedNames.count(firstB.getName()));
    ASSERT(persistedNames.count(thirdA.getName()));
    ASSERT(persistedNames.count(thirdB.getName()));
}

TEST_F(CommitCollectionMetadataLocallyTest,
       ChunkOperationsLeavesGappedNeighboursWhenNewChunkFallsInGap) {
    // The shard owns [0,10) and [30,40), leaving a gap in between. The stored chunks do not tile
    // the whole key space.
    auto [collType, _] = makeCollectionMetadata(0);
    auto ownedVersion = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {1, 0});
    auto owned1 = makeChunk(collType, key(0), key(10), ownedVersion);
    ownedVersion.incMajor();
    auto owned2 = makeChunk(collType, key(30), key(40), ownedVersion);
    mockCatalogClient()->setCollectionMetadata(collType, {owned1, owned2});
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    // A new chunk [15,25) sits entirely inside the gap, so it overlaps neither neighbour. Nothing
    // should be deleted: the new chunk is simply added next to the untouched [0,10) and [30,40).
    auto newVersion = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {3, 0});
    auto newChunk = makeChunk(collType, key(15), key(25), newVersion);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector({newChunk}));

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    std::set<OID> persistedNames;
    for (const auto& doc : chunkDocs) {
        persistedNames.insert(doc.getField(ChunkType::name.name()).OID());
    }
    ASSERT_EQ(chunkDocs.size(), 3u);
    ASSERT(persistedNames.count(owned1.getName()));    // [0,10) untouched.
    ASSERT(persistedNames.count(owned2.getName()));    // [30,40) untouched.
    ASSERT(persistedNames.count(newChunk.getName()));  // [15,25) added.
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsDeletesChunkExtendingPastNewRange) {
    // The shard owns [2,10) and [20,30).
    auto [collType, _] = makeCollectionMetadata(0);
    auto ownedVersion = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {1, 0});
    auto owned1 = makeChunk(collType, key(2), key(10), ownedVersion);
    ownedVersion.incMajor();
    auto owned2 = makeChunk(collType, key(20), key(30), ownedVersion);
    mockCatalogClient()->setCollectionMetadata(collType, {owned1, owned2});
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    // The new chunk [2,6) starts inside [2,10) but ends before it: [2,10) starts in the new range
    // yet extends past its max. It overlaps and must be deleted; [20,30), which starts past the
    // new range, is untouched. (The leftover [6,10) span is a gap the operation intends to leave.)
    auto newVersion = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {3, 0});
    auto newChunk = makeChunk(collType, key(2), key(6), newVersion);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector({newChunk}));

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    std::set<OID> persistedNames;
    for (const auto& doc : chunkDocs) {
        persistedNames.insert(doc.getField(ChunkType::name.name()).OID());
    }
    ASSERT_EQ(chunkDocs.size(), 2u);
    ASSERT(!persistedNames.count(owned1.getName()));   // [2,10) deleted (extends past new range).
    ASSERT(persistedNames.count(owned2.getName()));    // [20,30) untouched.
    ASSERT(persistedNames.count(newChunk.getName()));  // [2,6) added.
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsDeletesChunkStraddlingNewRangeStart) {
    // The shard owns [0,20) and [40,50).
    auto [collType, _] = makeCollectionMetadata(0);
    auto ownedVersion = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {1, 0});
    auto owned1 = makeChunk(collType, key(0), key(20), ownedVersion);
    ownedVersion.incMajor();
    auto owned2 = makeChunk(collType, key(40), key(50), ownedVersion);
    mockCatalogClient()->setCollectionMetadata(collType, {owned1, owned2});
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    // The new chunk [5,10) starts strictly inside [0,20): [0,20)'s min is below the new range's min
    // yet its max extends past it. This is the "chunk starting before the range" overlap case, so
    // [0,20) must be deleted; [40,50), which starts past the new range, is untouched. (The leftover
    // [0,5) and [10,20) spans are gaps the operation intends to leave.)
    auto newVersion = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {3, 0});
    auto newChunk = makeChunk(collType, key(5), key(10), newVersion);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector({newChunk}));

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    std::set<OID> persistedNames;
    for (const auto& doc : chunkDocs) {
        persistedNames.insert(doc.getField(ChunkType::name.name()).OID());
    }
    ASSERT_EQ(chunkDocs.size(), 2u);
    ASSERT(!persistedNames.count(owned1.getName()));  // [0,20) deleted (straddles new range start).
    ASSERT(persistedNames.count(owned2.getName()));   // [40,50) untouched.
    ASSERT(persistedNames.count(newChunk.getName()));  // [5,10) added.
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsDeltaOplogEntryIsIdempotentOnSecondary) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    boost::optional<CollectionMetadata> originalMetadata;
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        originalMetadata = scopedCsr->getCurrentMetadataIfKnown();
    }
    ASSERT_TRUE(originalMetadata);

    auto splitChunks = makeSplitChunks(collType, chunks[0]);
    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(splitChunks));
    auto oplogEntry = repl::OplogEntry(findLastOplogEntry());

    CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss)
        ->setCollectionMetadata(operationContext(), std::move(*originalMetadata));

    ShardServerOpObserver observer;
    observer.onUpdateCollectionMetadata(operationContext(), oplogEntry);
    observer.onUpdateCollectionMetadata(operationContext(), oplogEntry);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_EQ(metadata->getChunkManager()->numChunks(), 2);
    ASSERT_EQ(metadata->getCollPlacementVersion(), splitChunks.back().getVersion());
}

// A secondary applies chunk-operation deltas by replaying the oplog entry through
// onUpdateCollectionMetadata against its own CSR. If that CSR is a legacy full routing table (does
// not allow gaps), merging a delta that discards the lower-bound chunk would be rejected as
// inconsistent -- fatal during oplog application. The op observer must convert the base to a
// gap-allowing table first and then merge. The result keeps the other shard's chunks (a
// non-canonical full table with gaps) but routes correctly, and is corrected on the next full
// recovery. Each node makes this decision from its own CSR.
TEST_F(CommitCollectionMetadataLocallyTest,
       OnUpdateCollectionMetadataMergesDeltaOntoLegacyFullTableCsr) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    // Produce a delta oplog entry that hands this shard the middle range [50, 100). The CSR is left
    // unknown here (not bootstrapped): the commit only needs to emit the oplog entry, and applying
    // it on the primary is a no-op on an unknown base, so the legacy CSR installed below starts
    // fresh at a version older than the delta.
    auto deltaVersion = chunks.back().getVersion();
    deltaVersion.incMajor();
    auto deltaChunk = makeChunk(collType, key(50), key(100), deltaVersion, kMyShardName);
    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector({deltaChunk}));
    auto oplogEntry = repl::OplogEntry(findLastOplogEntry());
    ASSERT_EQ(oplogEntry.getObject().firstElementFieldNameStringData(), "updateCollectionMetadata");

    // Put this node's CSR into a legacy full-table shape owned entirely by another shard:
    // [MinKey, 100) and [100, MaxKey), contiguous and not allowing gaps.
    auto baseVersion = ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {1, 0});
    auto base1 = makeChunk(collType, key(MINKEY), key(100), baseVersion, ShardId("0"));
    baseVersion.incMinor();
    auto base2 = makeChunk(collType, key(100), key(MAXKEY), baseVersion, ShardId("0"));
    installLegacyFullTableCsr(collType, {base1, base2});

    // Replay the delta as a secondary would. This must not throw.
    ShardServerOpObserver observer;
    observer.onUpdateCollectionMetadata(operationContext(), oplogEntry);

    // The delta was merged onto the (converted) gap-allowing base rather than cleared: this shard
    // now owns [50, 100), the other shard's [100, MaxKey) is retained, and [MinKey, 50) is a gap.
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->allowGaps());
    ASSERT_EQ(metadata->getChunkManager()->numChunks(), 2);
    ASSERT(metadata->keyBelongsToMe(key(70)));    // owned: [50, 100)
    ASSERT(!metadata->keyBelongsToMe(key(150)));  // other shard: [100, MaxKey)
    ASSERT(!metadata->keyBelongsToMe(key(10)));   // gap: [MinKey, 50)
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsBootstrapsLocalCollectionMetadata) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    auto splitChunks = makeSplitChunks(collType, chunks[0]);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(splitChunks));

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), collType.getUuid());
}

// A migration recipient that currently owns no chunks has a known-but-unowned CSR (no routing
// table). It is flagged as receiving its first chunk, so the commit bootstraps the collection
// metadata from the global catalog rather than applying a delta on top of the absent routing table.
// This leaves the recipient primary with a usable routing table right away, without forcing a lazy
// recovery on the next access.
TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsHandlesUnownedCsr) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    // Put the CSR into the kUnowned state: known metadata, but no routing table.
    CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss)
        ->setCollectionMetadata(operationContext(),
                                CollectionMetadata{},
                                CollectionShardingRuntime::NoRoutingTableAs::kUnowned);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        ASSERT_FALSE(metadata->hasRoutingTable());
    }

    // Receiving the first chunk bootstraps from the global catalog. It must not throw.
    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(chunks), true /* receivingFirstChunk */);

    // The metadata is durable, and the in-memory metadata is installed as known (with a routing
    // table) from the global catalog instead of left cleared.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 1);
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->hasRoutingTable());
    ASSERT_EQ(metadata->getChunkManager()->numChunks(), 1);
    ASSERT_EQ(metadata->getCollPlacementVersion(), chunks.back().getVersion());
}

// A donor that gives up its last chunk must end with a correct, known CSR: it still knows the
// collection's routing table and version, but owns no chunks (tracked-unowned, shard placement
// version unset). It must not be left owning the migrated chunk, nor cleared to unknown.
TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsHandlesDonorGivingUpLastChunk) {
    const auto myShardId = ShardingState::get(operationContext())->shardId();

    // Build a collection whose single chunk is owned by this shard.
    auto [collType, _] = makeCollectionMetadata(1);
    auto ownedChunk =
        makeChunk(collType,
                  key(MINKEY),
                  key(MAXKEY),
                  ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {1, 0}),
                  myShardId);
    mockCatalogClient()->setCollectionMetadata(collType, {ownedChunk});

    // Bootstrap the CSR owning its single chunk.
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        ASSERT_TRUE(metadata->getShardPlacementVersion().isSet());
    }

    // The single chunk is migrated to another shard with a bumped version, at the same generation;
    // this shard keeps nothing.
    auto migratedVersion = ownedChunk.getVersion();
    migratedVersion.incMajor();
    auto migratedChunk = makeChunk(
        collType, ownedChunk.getMin(), ownedChunk.getMax(), migratedVersion, ShardId("otherShard"));

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector({migratedChunk}));

    // The CSR is known with a routing table but owns no chunks (tracked-unowned), at the new
    // collection version.
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->hasRoutingTable());
    ASSERT_FALSE(scopedCsr->isUnowned());
    ASSERT_FALSE(metadata->getShardPlacementVersion().isSet());
    ASSERT_EQ(metadata->getCollPlacementVersion(), migratedChunk.getVersion());
}

// This test verifies that chunks are inserted before collections. This is necessary because
// cleanup removes collection entries with no chunks. Inserting the collection entry first
// could race with cleanup and cause it to be removed.
TEST_F(CommitCollectionMetadataLocallyTest,
       ChunkOperationsBootstrapsCollectionMetadataAfterChunks) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    auto writeOrderObserver = installWriteOrderObserver();

    auto realChunk =
        makeCoveringChunks(collType,
                           1 /* nChunks */,
                           ChunkVersion({collType.getEpoch(), collType.getTimestamp()}, {0, 0}));

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(realChunk));

    ASSERT_EQ(writeOrderObserver->writes().size(), 2u);
    ASSERT_EQ(writeOrderObserver->writes()[0], NamespaceString::kConfigShardCatalogChunksNamespace);
    ASSERT_EQ(writeOrderObserver->writes()[1],
              NamespaceString::kConfigShardCatalogCollectionsNamespace);
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunklessCollectionPersistsTokenToDisk) {
    auto [collType, chunks] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), collType.getUuid());

    // No chunk should be stored in the local chunks for a chunkless collection on the dbPrimary.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunklessCollectionUpdatesCSR) {
    auto [collType, chunks] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->isSharded());
    ASSERT_EQ(metadata->getChunkManager()->getUUID(), collType.getUuid());
    // The DB primary owns no real chunks for this collection, so its placement version is unset.
    ASSERT_FALSE(metadata->getShardPlacementVersion().isSet());
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunklessCollectionIsIdempotent) {
    auto [collType, _] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyChunklessPersistsCollectionWithNewEpoch) {
    // Seed a chunkless tracked collection at (epoch1, ts1).
    auto [collType1, _] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType1, {});
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);

    // Simulate a refine to (epoch2, ts2) on the same UUID with an extended key pattern.
    const OID epoch2 = OID::gen();
    const Timestamp ts2 = collType1.getTimestamp() + 1;
    const BSONObj newKeyPattern = BSON("_id" << 1 << "extra" << 1);
    CollectionType collType2{
        kTestNss, epoch2, ts2, Date_t::now(), collType1.getUuid(), newKeyPattern};
    mockCatalogClient()->setCollectionMetadata(collType2, {});

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_BSONOBJ_EQ(collDocs[0].getObjectField("key"), newKeyPattern);
}

TEST_F(CommitCollectionMetadataLocallyTest,
       CommitChunklessUpdatesCatalogButPreservesExistingChunklessCSR) {
    auto [collType1, _] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType1, {});
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        ASSERT_TRUE(metadata->hasRoutingTable());
        ASSERT_FALSE(metadata->getShardPlacementVersion().isSet());
    }

    const OID epoch2 = OID::gen();
    const Timestamp ts2 = collType1.getTimestamp() + 1;
    const BSONObj newKeyPattern = BSON("_id" << 1 << "extra" << 1);
    CollectionType collType2{
        kTestNss, epoch2, ts2, Date_t::now(), collType1.getUuid(), newKeyPattern};
    mockCatalogClient()->setCollectionMetadata(collType2, {});

    shard_catalog_commit::commitChunklessCollectionMetadataLocally(operationContext(), kTestNss);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_BSONOBJ_EQ(collDocs[0].getObjectField("key"), newKeyPattern);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->hasRoutingTable());
    ASSERT_FALSE(metadata->getShardPlacementVersion().isSet());
    ASSERT_BSONOBJ_EQ(metadata->getChunkManager()->getShardKeyPattern().getKeyPattern().toBSON(),
                      kShardKeyPattern);
    ASSERT_EQ(metadata->getChunkManager()->getVersion().epoch(), collType1.getEpoch());
    ASSERT_EQ(metadata->getChunkManager()->getVersion().getTimestamp(), collType1.getTimestamp());
}

TEST_F(CommitCollectionMetadataLocallyTest, CommitChunklessPreservesCSRWithOwnedChunks) {
    auto [collType1, chunks] = makeCollectionMetadata(2);
    for (auto& chunk : chunks) {
        chunk.setShard(kMyShardName);
    }
    mockCatalogClient()->setCollectionMetadata(collType1, chunks);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, false);

    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        ASSERT_TRUE(metadata->getShardPlacementVersion().isSet());
    }

    const OID epoch2 = OID::gen();
    const Timestamp ts2 = collType1.getTimestamp() + 1;
    const BSONObj newKeyPattern = BSON("_id" << 1 << "extra" << 1);
    CollectionType collType2{
        kTestNss, epoch2, ts2, Date_t::now(), collType1.getUuid(), newKeyPattern};
    mockCatalogClient()->setCollectionMetadata(collType2, {});

    shard_catalog_commit::commitChunklessCollectionMetadataLocally(operationContext(), kTestNss);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_BSONOBJ_EQ(collDocs[0].getObjectField("key"), newKeyPattern);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->getShardPlacementVersion().isSet());
    ASSERT_BSONOBJ_EQ(metadata->getChunkManager()->getShardKeyPattern().getKeyPattern().toBSON(),
                      kShardKeyPattern);
    ASSERT_EQ(metadata->getChunkManager()->getVersion().epoch(), collType1.getEpoch());
    ASSERT_EQ(metadata->getChunkManager()->getVersion().getTimestamp(), collType1.getTimestamp());
}

TEST_F(CommitCollectionMetadataLocallyTest, CommitMarksOpCtxNonDeprioritizable) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    // The shard catalog commit holds the critical section, so it must mark the opCtx as
    // non-deprioritizable rather than relying on the invoking command to do so.
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    ASSERT_TRUE(ExecutionAdmissionContext::get(operationContext()).getMarkedNonDeprioritizable());
}

TEST_F(CommitCollectionMetadataLocallyTest, CommitChunklessMarksOpCtxNonDeprioritizable) {
    auto [collType, _] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});

    shard_catalog_commit::commitChunklessCollectionMetadataLocally(operationContext(), kTestNss);

    ASSERT_TRUE(ExecutionAdmissionContext::get(operationContext()).getMarkedNonDeprioritizable());
}

TEST_F(CommitCollectionMetadataLocallyTest, DropCollectionMarksOpCtxNonDeprioritizable) {
    auto uuid = UUID::gen();

    shard_catalog_commit::commitDropCollectionLocally(operationContext(), kTestNss, uuid);

    ASSERT_TRUE(ExecutionAdmissionContext::get(operationContext()).getMarkedNonDeprioritizable());
}

TEST_F(CommitCollectionMetadataLocallyTest, RenameMarksOpCtxNonDeprioritizable) {
    mockCatalogClient()->setCollectionNotFound();

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           boost::none,
                                                           kToNss,
                                                           boost::none,
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimaryShard */);

    ASSERT_TRUE(ExecutionAdmissionContext::get(operationContext()).getMarkedNonDeprioritizable());
}

// The clone path runs outside the critical section, so it must not be marked non-deprioritizable.
TEST_F(CommitCollectionMetadataLocallyTest, CloneDoesNotMarkOpCtxNonDeprioritizable) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::cloneCollectionMetadataLocally(
        operationContext(), kTestNss, true /* isDbPrimaryShard */);

    ASSERT_FALSE(ExecutionAdmissionContext::get(operationContext()).getMarkedNonDeprioritizable());
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunkOperationsMarksOpCtxNonDeprioritizable) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    auto splitChunks = makeSplitChunks(collType, chunks[0]);

    shard_catalog_commit::commitChunkOperationsMetadataLocally(
        operationContext(), kTestNss, toConfigBSONVector(splitChunks));

    ASSERT_TRUE(ExecutionAdmissionContext::get(operationContext()).getMarkedNonDeprioritizable());
}

TEST_F(CommitCollectionMetadataLocallyTest,
       DropOfStaleChunksForRenameDoesNotMarkOpCtxNonDeprioritizable) {
    shard_catalog_commit::commitDropOfStaleChunksForRename(operationContext(), UUID::gen());

    ASSERT_FALSE(ExecutionAdmissionContext::get(operationContext()).getMarkedNonDeprioritizable());
}

// The chunkless commit emits a 'c' entry carrying the kIfUnowned precondition, delegating the
// decision to clear to each node. A locally tracked node does not satisfy the precondition, so its
// filtering metadata is preserved.
TEST_F(CommitCollectionMetadataLocallyTest,
       CommitChunklessAlwaysEmitsInvalidateWithIfUnownedConditionAndPreservesTrackedCsr) {
    auto [collType, _] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
        ASSERT_FALSE(scopedCsr->isUnowned());
    }

    shard_catalog_commit::commitChunklessCollectionMetadataLocally(operationContext(), kTestNss);

    // The invalidate is always emitted and only clears UNOWNED nodes.
    auto oplogEntry = findLastOplogEntry();
    auto object = oplogEntry.getObjectField("o");
    ASSERT_EQ(object.firstElementFieldNameStringData(), "invalidateCollectionMetadata");
    ASSERT_TRUE(object.getBoolField("onlyClearIfUnowned"));

    // This node is tracked, so it does not satisfy the precondition and keeps its metadata.
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
}

// When this node's CSS is UNOWNED, applying the chunkless commit's invalidate locally clears the
// filtering metadata so the next access recovers it as tracked.
TEST_F(CommitCollectionMetadataLocallyTest, CommitChunklessClearsCsrWhenLocallyUnowned) {
    auto [collType, _] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});

    CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss)
        ->setCollectionMetadata(operationContext(),
                                CollectionMetadata::UNTRACKED(),
                                CollectionShardingRuntime::NoRoutingTableAs::kUnowned);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_TRUE(scopedCsr->isUnowned());
    }

    shard_catalog_commit::commitChunklessCollectionMetadataLocally(operationContext(), kTestNss);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
}

// Directly exercises the op observer to prove the kIfUnowned precondition is evaluated per-node:
// the same invalidate 'c' entry clears an UNOWNED node's metadata but leaves a tracked node's
// metadata untouched.
TEST_F(CommitCollectionMetadataLocallyTest, OnInvalidateIfUnownedClearsOnlyLocallyUnownedNodes) {
    // Produce a real kIfUnowned invalidate 'c' entry and capture it to replay as a secondary would.
    auto [chunkless, _] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(chunkless, {});
    shard_catalog_commit::commitChunklessCollectionMetadataLocally(operationContext(), kTestNss);

    const auto invalidateEntry = repl::OplogEntry(findLastOplogEntry());
    ASSERT_EQ(invalidateEntry.getObject().firstElementFieldNameStringData(),
              "invalidateCollectionMetadata");
    ASSERT_TRUE(invalidateEntry.getObject().getBoolField("onlyClearIfUnowned"));

    ShardServerOpObserver observer;

    // A node whose CSS is tracked does not satisfy the precondition: replaying the entry leaves its
    // filtering metadata untouched.
    {
        auto [trackedColl, chunks] = makeCollectionMetadata(2);
        for (auto& chunk : chunks) {
            chunk.setShard(kMyShardName);
        }
        mockCatalogClient()->setCollectionMetadata(trackedColl, chunks);
        shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, false);
        {
            auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
            ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
        }

        observer.onInvalidateCollectionMetadata(operationContext(), invalidateEntry);

        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
    }

    // A node whose CSS is unknown does not satisfy the precondition: replaying the entry leaves
    // its filtering metadata untouched.
    {
        CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss)
            ->clearCollectionMetadata(operationContext());

        observer.onInvalidateCollectionMetadata(operationContext(), invalidateEntry);

        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
    }

    // A node whose CSS is UNTRACKED (unsharded) does not satisfy the precondition: replaying the
    // entry leaves its filtering metadata untouched.
    {
        CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss)
            ->setCollectionMetadata(operationContext(),
                                    CollectionMetadata::UNTRACKED(),
                                    CollectionShardingRuntime::NoRoutingTableAs::kUntracked);

        observer.onInvalidateCollectionMetadata(operationContext(), invalidateEntry);

        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        ASSERT_FALSE(metadata->isSharded());
        ASSERT_EQ(metadata->getShardPlacementVersion(), ChunkVersion::UNTRACKED());
    }

    // A node whose CSS is UNOWNED satisfies the precondition: replaying the entry clears it.
    {
        CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss)
            ->setCollectionMetadata(operationContext(),
                                    CollectionMetadata::UNTRACKED(),
                                    CollectionShardingRuntime::NoRoutingTableAs::kUnowned);

        observer.onInvalidateCollectionMetadata(operationContext(), invalidateEntry);

        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
    }
}

// A versioned invalidate leaves an unknown CSR untouched: there is no in-memory metadata to
// reconcile against the disk shard version carried by the entry.
TEST_F(CommitCollectionMetadataLocallyTest, OnInvalidateWithShardVersionKeepsUnknownCsr) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    chunks[0].setShard(kMyShardName);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::cloneCollectionMetadataLocally(
        operationContext(), kTestNss, false /* isDbPrimaryShard */);
    const auto invalidateEntry = repl::OplogEntry(findLastOplogEntry());

    CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss)
        ->clearCollectionMetadata(operationContext());

    ShardServerOpObserver observer;
    observer.onInvalidateCollectionMetadata(operationContext(), invalidateEntry);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
}

// A versioned invalidate leaves a tracked CSR untouched when its shard version already matches
// the disk shard version carried by the entry.
TEST_F(CommitCollectionMetadataLocallyTest, OnInvalidateWithShardVersionKeepsUpToDateTrackedCsr) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    chunks[0].setShard(kMyShardName);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, false);

    const auto invalidateEntry = makeInvalidateOplogEntry(collType.getUuid(), [&] {
        InvalidateCollectionMetadataOplogEntry entry{std::string(kTestNss.coll())};
        entry.setShardVersion(chunks[0].getVersion());
        return entry;
    }());

    ShardServerOpObserver observer;
    observer.onInvalidateCollectionMetadata(operationContext(), invalidateEntry);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_EQ(metadata->getShardPlacementVersion(), chunks[0].getVersion());
}

// A versioned invalidate clears a tracked CSR whose in-memory shard version is older than the disk
// shard version carried by the entry.
TEST_F(CommitCollectionMetadataLocallyTest, OnInvalidateWithShardVersionClearsStaleTrackedCsr) {
    auto [collType, chunksV1] = makeCollectionMetadata(1);
    chunksV1[0].setShard(kMyShardName);
    mockCatalogClient()->setCollectionMetadata(collType, chunksV1);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, false);

    auto chunksV2 = makeCoveringChunks(collType, 2 /* nChunks */, chunksV1[0].getVersion());
    for (auto& chunk : chunksV2) {
        chunk.setShard(kMyShardName);
    }
    const auto invalidateEntry = makeInvalidateOplogEntry(collType.getUuid(), [&] {
        InvalidateCollectionMetadataOplogEntry entry{std::string(kTestNss.coll())};
        entry.setShardVersion(chunksV2.back().getVersion());
        return entry;
    }());

    ShardServerOpObserver observer;
    observer.onInvalidateCollectionMetadata(operationContext(), invalidateEntry);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
}

// A versioned invalidate still clears an UNOWNED CSR: unowned nodes have no durable metadata and
// must always be eligible to recover from disk.
TEST_F(CommitCollectionMetadataLocallyTest, OnInvalidateWithShardVersionClearsUnownedCsr) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    chunks[0].setShard(kMyShardName);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss)
        ->setCollectionMetadata(operationContext(),
                                CollectionMetadata::UNTRACKED(),
                                CollectionShardingRuntime::NoRoutingTableAs::kUnowned);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_TRUE(scopedCsr->isUnowned());
    }

    const auto invalidateEntry = makeInvalidateOplogEntry(collType.getUuid(), [&] {
        InvalidateCollectionMetadataOplogEntry entry{std::string(kTestNss.coll())};
        entry.setShardVersion(chunks[0].getVersion());
        return entry;
    }());

    ShardServerOpObserver observer;
    observer.onInvalidateCollectionMetadata(operationContext(), invalidateEntry);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
}

// A versioned invalidate leaves UNTRACKED filtering metadata untouched when the entry's disk
// shard version is also UNTRACKED: both sides agree the collection is unsharded, so there is
// nothing to reconcile.
TEST_F(CommitCollectionMetadataLocallyTest, OnInvalidateWithShardVersionKeepsUntrackedCsr) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss)
        ->setCollectionMetadata(operationContext(),
                                CollectionMetadata::UNTRACKED(),
                                CollectionShardingRuntime::NoRoutingTableAs::kUntracked);

    const auto invalidateEntry = makeInvalidateOplogEntry(collType.getUuid(), [&] {
        InvalidateCollectionMetadataOplogEntry entry{std::string(kTestNss.coll())};
        entry.setShardVersion(ChunkVersion::UNTRACKED());
        return entry;
    }());

    ShardServerOpObserver observer;
    observer.onInvalidateCollectionMetadata(operationContext(), invalidateEntry);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_FALSE(metadata->isSharded());
    ASSERT_EQ(metadata->getShardPlacementVersion(), ChunkVersion::UNTRACKED());
}

// An unconditional invalidate (no shardVersion) clears even an unknown CSR.
TEST_F(CommitCollectionMetadataLocallyTest, OnInvalidateUnconditionalClearsUnknownCsr) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    const auto invalidateEntry = makeInvalidateOplogEntry(
        collType.getUuid(), InvalidateCollectionMetadataOplogEntry{std::string(kTestNss.coll())});

    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
    }

    ShardServerOpObserver observer;
    observer.onInvalidateCollectionMetadata(operationContext(), invalidateEntry);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
}

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyRemovesStaleChunks) {
    auto [collType, chunks] = makeCollectionMetadata(2);

    // Pre-populate with stale chunks that have a different number of shard key fields.
    {
        DBDirectClient client(operationContext());
        for (const auto& chunk : chunks) {
            auto bson = chunk.toConfigBSON();
            // Insert a stale chunk with a compound min key (2 fields vs the 1-field shard key).
            BSONObjBuilder staleDoc;
            for (const auto& elem : bson) {
                if (elem.fieldNameStringData() == "min") {
                    staleDoc.append("min", BSON("_id" << 1 << "extra" << 1));
                } else if (elem.fieldNameStringData() == "max") {
                    staleDoc.append("max", BSON("_id" << 2 << "extra" << 2));
                } else {
                    staleDoc.append(elem);
                }
            }
            client.insert(NamespaceString::kConfigShardCatalogChunksNamespace, staleDoc.obj());
        }
        ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);
    }

    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    // The stale chunks should have been removed and replaced with the correct ones.
    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    ASSERT_EQ(chunkDocs.size(), 2u);
    for (const auto& doc : chunkDocs) {
        ASSERT_EQ(doc.getObjectField("min").nFields(), 1);
    }
}

TEST_F(CommitCollectionMetadataLocallyTest, DropCollectionDeletesMetadata) {
    auto [collType, chunks] = makeCollectionMetadata(3);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    // First commit the metadata so there's something to drop.
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    shard_catalog_commit::commitDropCollectionLocally(
        operationContext(), kTestNss, collType.getUuid());

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 0);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);

    // The drop is counted distinctly from the commit that seeded the metadata.
    auto stats = getRecoveryStats();
    ASSERT_EQ(stats.getIntField("countLocalCollectionMetadataDrops"), 1);
    ASSERT_EQ(stats.getIntField("countLocalCollectionMetadataCommits"), 1);
    ASSERT_EQ(stats.getIntField("countLocalCollectionMetadataRenames"), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, DropCollectionClearsCSR) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
    }

    shard_catalog_commit::commitDropCollectionLocally(
        operationContext(), kTestNss, collType.getUuid());

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_FALSE(metadata) << "CSR should have no metadata after drop";
}

TEST_F(CommitCollectionMetadataLocallyTest, CommitNotifiesInFlightRecoverer) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    // Simulate a recovery round that has already read from disk and is waiting to drain.
    auto recoverer = std::make_shared<CollectionCacheRecoverer>(
        kTestNss, CancellationToken::uncancelable(), CollectionMetadata::UNTRACKED());
    {
        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss);
        scopedCsr->setCollectionRecoverer(recoverer);
    }
    auto roundId = recoverer->start(operationContext(), nullptr);

    // Drop the collection. This should notify the recoverer about the drop.
    shard_catalog_commit::commitDropCollectionLocally(
        operationContext(), kTestNss, collType.getUuid());

    // The recoverer should force a new recovery instead of returning the metadata before the drop.
    ASSERT_FALSE(recoverer->drainAndApply(operationContext(), roundId));
}

TEST_F(CommitCollectionMetadataLocallyTest, DropCollectionIsNoOpOnEmptyCatalog) {
    auto uuid = UUID::gen();
    shard_catalog_commit::commitDropCollectionLocally(operationContext(), kTestNss, uuid);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 0);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, DropCollectionOnlyDeletesTargetCollection) {
    auto [collType1, chunks1] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType1, chunks1);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    // Insert a second collection's data directly.
    auto otherNss = NamespaceString::createNamespaceString_forTest("TestDB", "OtherColl");
    auto [collType2, chunks2] = [&] {
        const UUID uuid = UUID::gen();
        const OID epoch = OID::gen();
        const Timestamp ts(Date_t::now());
        CollectionType coll{otherNss, epoch, ts, Date_t::now(), uuid, kShardKeyPattern};
        ChunkType chunk(uuid,
                        ChunkRange(key(MINKEY), key(MAXKEY)),
                        ChunkVersion({epoch, ts}, {1, 0}),
                        ShardId("0"));
        chunk.setName(OID::gen());
        return CollectionAndChunksMetadata{std::move(coll), {std::move(chunk)}};
    }();
    {
        DBDirectClient client(operationContext());
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType2.toBSON());
        for (const auto& chunk : chunks2) {
            client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                          chunk.toConfigBSON());
        }
    }

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 2);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    shard_catalog_commit::commitDropCollectionLocally(
        operationContext(), kTestNss, collType1.getUuid());

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 1);

    auto remainingColls = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(UUID::fromCDR(remainingColls[0].getField("uuid").uuid()), collType2.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest,
       DropCollectionMetadataInvalidationOplogEntryUsesCommandNamespace) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    seedShardCatalog(collType, chunks);

    shard_catalog_commit::commitDropCollectionLocally(
        operationContext(), kTestNss, collType.getUuid());

    auto oplogEntries =
        findLocalDocs(NamespaceString::kRsOplogNamespace,
                      BSON("op" << "c" << "o.invalidateCollectionMetadata" << kTestNss.coll()));
    ASSERT_EQ(oplogEntries.size(), 1u);
    ASSERT_EQ(oplogEntries.front().getStringField("ns"), kTestNss.getCommandNS().ns_forTest());
}

TEST_F(CommitCollectionMetadataLocallyTest, SetAllowChunkOperationsOplogEntryUsesCommandNamespace) {
    unittest::ServerParameterGuard authoritativeScope("featureFlagAuthoritativeShardsCRUD", true);

    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    auto shardVersion = [&] {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        return ShardVersionFactory::make(*metadata);
    }();
    ScopedSetShardRole scopedSetShardRole{
        operationContext(), kTestNss, shardVersion, boost::none /* databaseVersion */};

    shard_catalog_commit::commitSetAllowChunkOperationsLocally(
        operationContext(), kTestNss, false /* allowChunkOperations */, collType.getUuid());

    auto oplogEntries =
        findLocalDocs(NamespaceString::kRsOplogNamespace,
                      BSON("op" << "c" << "o.setAllowChunkOperations" << kTestNss.coll()));
    ASSERT_EQ(oplogEntries.size(), 2u);
    ASSERT_EQ(oplogEntries.back().getStringField("ns"), kTestNss.getCommandNS().ns_forTest());
}

// ---------------------------------------------------------------------------
// Clone (setFCV) tests
// ---------------------------------------------------------------------------

// The clone persists the durable shard catalog and emits a single kIfStale invalidate 'c' entry
// (carrying the freshly persisted shard version) but does not emit setAllowChunkOperations and does
// not install metadata in-memory: with no prior in-memory state there is nothing stale to clear.
TEST_F(CommitCollectionMetadataLocallyTest, CloneEmitsIfStaleInvalidateWithoutInstalling) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::cloneCollectionMetadataLocally(
        operationContext(), kTestNss, true /* isDbPrimaryShard */);

    // The durable shard catalog is written.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    // The clone is counted and is not double-counted as a commit.
    ASSERT_EQ(getRecoveryStats().getIntField("countLocalCollectionMetadataClones"), 1);
    ASSERT_EQ(getRecoveryStats().getIntField("countLocalCollectionMetadataCommits"), 0);

    // The clone emits exactly one invalidate carrying the disk shard version, and never a
    // setAllowChunkOperations (the metadata is not reinstalled in-memory).
    ASSERT_EQ(countCommandOplogEntries("invalidateCollectionMetadata", kTestNss), 1);
    ASSERT_EQ(countCommandOplogEntries("setAllowChunkOperations", kTestNss), 0);
    auto oplogEntry = findLastOplogEntry();
    auto object = oplogEntry.getObjectField("o");
    ASSERT_EQ(object.firstElementFieldNameStringData(), "invalidateCollectionMetadata");
    ASSERT_TRUE(object.hasField("shardVersion"));
    ASSERT_FALSE(object.getBoolField("onlyClearIfUnowned"));

    // With no prior in-memory state, the kIfStale precondition does not hold, so the CSR stays
    // unknown, to be recovered lazily from the durable catalog when next needed.
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
}

// The clone clears an in-memory CSR whose shard version is older than the one it persists, so the
// stale legacy state gets re-recovered from the authoritative catalog.
TEST_F(CommitCollectionMetadataLocallyTest, CloneClearsKnownButStaleCsr) {
    // Install a CSR at an older shard version owned by this shard.
    auto [collType, chunksV1] = makeCollectionMetadata(1);
    chunksV1[0].setShard(kMyShardName);
    mockCatalogClient()->setCollectionMetadata(collType, chunksV1);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, false);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
    }

    // The authoritative catalog now holds a newer shard version (same collection generation).
    auto chunksV2 = makeCoveringChunks(collType, 2 /* nChunks */, chunksV1[0].getVersion());
    for (auto& chunk : chunksV2) {
        chunk.setShard(kMyShardName);
    }
    mockCatalogClient()->setCollectionMetadata(collType, chunksV2);

    shard_catalog_commit::cloneCollectionMetadataLocally(
        operationContext(), kTestNss, false /* isDbPrimaryShard */);

    // The in-memory shard version was older than the cloned one, so the CSR was cleared.
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
}

// The clone leaves an in-memory CSR untouched when its shard version already matches disk, avoiding
// a redundant refresh.
TEST_F(CommitCollectionMetadataLocallyTest, CloneKeepsKnownUpToDateCsr) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    chunks[0].setShard(kMyShardName);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, false);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
    }

    // Clone the identical metadata: the disk shard version equals the in-memory one, so kIfStale
    // does not hold and the CSR is preserved.
    shard_catalog_commit::cloneCollectionMetadataLocally(
        operationContext(), kTestNss, false /* isDbPrimaryShard */);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_EQ(metadata->getShardPlacementVersion(), chunks[0].getVersion());
}

// The clone leaves a chunkless tracked CSR untouched when both disk and in-memory carry unset
// placement versions for the same collection: those versions are uncomparable, so kIfStale does
// not clear.
TEST_F(CommitCollectionMetadataLocallyTest, CloneKeepsChunklessCsrWithUnsetPlacementVersion) {
    auto [collType, _] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        ASSERT_FALSE(metadata->getShardPlacementVersion().isSet());
    }

    shard_catalog_commit::cloneCollectionMetadataLocally(
        operationContext(), kTestNss, true /* isDbPrimaryShard */);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_FALSE(metadata->getShardPlacementVersion().isSet());
}

// The clone must not clear an existing in-memory CSR when the shard owns no chunks and is not the
// DB primary; the on-disk entry is still removed.
TEST_F(CommitCollectionMetadataLocallyTest, CloneDoesNotClearCsrWhenShardOwnsNoChunks) {
    // Seed an existing CSR with known metadata, as if it had been populated by the legacy catalog
    // cache loader prior to cloning. The non-clone commit emits exactly one invalidate 'c' entry,
    // which lets the assertion below verify the clone itself emits none.
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);
    ASSERT_EQ(countCommandOplogEntries("invalidateCollectionMetadata", kTestNss), 1);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
    }

    // Clone as a non-primary shard that owns no chunks: the durable entry is removed but the
    // in-memory CSR is left untouched (no clear) and no further oplog entry is emitted.
    mockCatalogClient()->setCollectionMetadata(collType, {} /* no owned chunks */);
    shard_catalog_commit::cloneCollectionMetadataLocally(
        operationContext(), kTestNss, false /* isDbPrimaryShard */);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 0);
    // Still just the single invalidate from the non-clone seed; the clone added none.
    ASSERT_EQ(countCommandOplogEntries("invalidateCollectionMetadata", kTestNss), 1);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
}

// ---------------------------------------------------------------------------
// Rename tests
// ---------------------------------------------------------------------------

TEST_F(CommitCollectionMetadataLocallyTest, RenameUnshardedToShardedReplacingIt) {
    // toNss holds a sharded collection; fromNss is unsharded (no shard catalog entry).
    auto [toCollType, toChunks] = makeCollectionMetadata(kToNss, 3);
    seedShardCatalog(toCollType, toChunks);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    // After rename, toNss holds the unsharded collection — CSRS has no entry for it.
    mockCatalogClient()->setCollectionNotFound();

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           boost::none,
                                                           kToNss,
                                                           toCollType.getUuid(),
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimary */);

    shard_catalog_commit::commitDropOfStaleChunksForRename(operationContext(),
                                                           toCollType.getUuid());

    // The renamed collection is unsharded so it has no catalog representation; the replaced
    // sharded collection is fully removed.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 0);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);

    // The rename primitive is counted distinctly.
    ASSERT_EQ(getRecoveryStats().getIntField("countLocalCollectionMetadataRenames"), 1);
}

TEST_F(CommitCollectionMetadataLocallyTest, RenameUnshardedToUnsharded) {
    // Both fromNss and toNss are unsharded — nothing to track in the shard catalog.
    mockCatalogClient()->setCollectionNotFound();

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           boost::none,
                                                           kToNss,
                                                           boost::none,
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimaryShard */);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 0);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, RenameShardedToUnshardedReplacingIt) {
    // fromNss is sharded; toNss is unsharded (gets replaced). After the rename the sharded
    // collection lives at toNss.
    auto [fromCollType, fromChunks] = makeCollectionMetadata(kFromNss, 3);
    seedShardCatalog(fromCollType, fromChunks);

    // The CSRS now shows the renamed collection under toNss with the same UUID.
    CollectionType renamedColl{kToNss,
                               fromCollType.getEpoch(),
                               fromCollType.getTimestamp(),
                               Date_t::now(),
                               fromCollType.getUuid(),
                               kShardKeyPattern};
    mockCatalogClient()->setCollectionMetadata(renamedColl, fromChunks);

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           fromCollType.getUuid(),
                                                           kToNss,
                                                           boost::none,
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimary */);

    // Metadata now lives at toNss; fromNss entry is gone; chunks are unchanged.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), fromCollType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest, RenameShardedToShardedReplacingIt) {
    // fromNss is sharded; toNss is also sharded and gets replaced by the rename.
    auto [fromCollType, fromChunks] = makeCollectionMetadata(kFromNss, 2);
    auto [toCollType, toChunks] = makeCollectionMetadata(kToNss, 3);
    seedShardCatalog(fromCollType, fromChunks);
    seedShardCatalog(toCollType, toChunks);

    // The CSRS now shows the renamed collection under toNss with fromNss's UUID.
    CollectionType renamedColl{kToNss,
                               fromCollType.getEpoch(),
                               fromCollType.getTimestamp(),
                               Date_t::now(),
                               fromCollType.getUuid(),
                               kShardKeyPattern};
    mockCatalogClient()->setCollectionMetadata(renamedColl, fromChunks);

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           fromCollType.getUuid(),
                                                           kToNss,
                                                           toCollType.getUuid(),
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimary */);

    shard_catalog_commit::commitDropOfStaleChunksForRename(operationContext(),
                                                           toCollType.getUuid());

    // toNss entry reflects fromNss metadata; fromNss entry is gone; replaced toNss chunks removed.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), fromCollType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest, RenameShardedToShardedNoReplacement) {
    // fromNss is sharded; toNss did not exist before the rename.
    auto [fromCollType, fromChunks] = makeCollectionMetadata(kFromNss, 2);
    seedShardCatalog(fromCollType, fromChunks);

    // The CSRS now shows the renamed collection under toNss with fromNss's UUID.
    CollectionType renamedColl{kToNss,
                               fromCollType.getEpoch(),
                               fromCollType.getTimestamp(),
                               Date_t::now(),
                               fromCollType.getUuid(),
                               kShardKeyPattern};
    mockCatalogClient()->setCollectionMetadata(renamedColl, fromChunks);

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           fromCollType.getUuid(),
                                                           kToNss,
                                                           boost::none,
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimary */);

    // toNss entry reflects fromNss metadata; fromNss entry is gone.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), fromCollType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest,
       RenameShardedToShardedNoReplacementDoesNotRewritePersistedChunks) {
    // UUID-preserving rename leaves durable chunk documents untouched; only the collection entry
    // moves to toNss.
    auto [fromCollType, fromChunks] = makeCollectionMetadata(kFromNss, 2);
    seedShardCatalog(fromCollType, fromChunks);
    const auto chunkDocsBefore = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    auto writeOrderObserver = installWriteOrderObserver();

    CollectionType renamedColl{kToNss,
                               fromCollType.getEpoch(),
                               fromCollType.getTimestamp(),
                               Date_t::now(),
                               fromCollType.getUuid(),
                               kShardKeyPattern};
    mockCatalogClient()->setCollectionMetadata(renamedColl, fromChunks);

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           fromCollType.getUuid(),
                                                           kToNss,
                                                           boost::none,
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimary */);

    ASSERT_EQ(writeOrderObserver->writes().size(), 1u);
    ASSERT_EQ(writeOrderObserver->writes()[0],
              NamespaceString::kConfigShardCatalogCollectionsNamespace);

    const auto chunkDocsAfter = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    ASSERT_EQ(chunkDocsBefore.size(), chunkDocsAfter.size());
    for (size_t i = 0; i < chunkDocsBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(chunkDocsBefore[i], chunkDocsAfter[i]);
    }
}

TEST_F(CommitCollectionMetadataLocallyTest, RenameRecoversTargetFilteringMetadataInMemory) {
    // fromNss is sharded with chunks owned by this shard; toNss did not exist before the rename.
    auto [fromCollType, fromChunks] = makeCollectionMetadata(kFromNss, 2);
    for (auto& chunk : fromChunks) {
        chunk.setShard(ShardingState::get(operationContext())->shardId());
    }
    seedShardCatalog(fromCollType, fromChunks);

    // The CSRS now shows the renamed collection under toNss with fromNss's UUID.
    CollectionType renamedColl{kToNss,
                               fromCollType.getEpoch(),
                               fromCollType.getTimestamp(),
                               Date_t::now(),
                               fromCollType.getUuid(),
                               kShardKeyPattern};
    mockCatalogClient()->setCollectionMetadata(renamedColl, fromChunks);

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           fromCollType.getUuid(),
                                                           kToNss,
                                                           boost::none,
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimary */);

    // The target filtering metadata is recovered in-memory right away instead of being deferred to
    // the first query.
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kToNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata) << "target metadata should have been recovered eagerly after the rename";
    ASSERT_TRUE(metadata->isSharded());
    ASSERT_EQ(metadata->getChunkManager()->getUUID(), fromCollType.getUuid());
    ASSERT_TRUE(metadata->getShardPlacementVersion().isSet());
}

TEST_F(CommitCollectionMetadataLocallyTest,
       RenameDbPrimaryWithoutOwnedChunksRecoversTrackedMetadata) {
    // toNss is tracked but this shard owns no chunks for it. As the db-primary it still installs
    // tracked (unowned) metadata in-memory rather than deferring to the first query.
    auto [fromCollType, unused] = makeCollectionMetadata(kFromNss, 0);

    CollectionType renamedColl{kToNss,
                               fromCollType.getEpoch(),
                               fromCollType.getTimestamp(),
                               Date_t::now(),
                               fromCollType.getUuid(),
                               kShardKeyPattern};
    mockCatalogClient()->setCollectionMetadata(renamedColl, {});

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           fromCollType.getUuid(),
                                                           kToNss,
                                                           boost::none,
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           true /* isDbPrimary */);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kToNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->hasRoutingTable());
    // The db-primary owns no real chunks for this collection, so its placement version is unset.
    ASSERT_FALSE(metadata->getShardPlacementVersion().isSet());
}

TEST_F(CommitCollectionMetadataLocallyTest, RenameUnownedNonPrimaryClearsTargetMetadata) {
    // toNss is tracked but this shard neither owns chunks nor is the db-primary, so it must not
    // hold any in-memory metadata for it nor persist a collection entry.
    auto [fromCollType, unused] = makeCollectionMetadata(kFromNss, 0);

    CollectionType renamedColl{kToNss,
                               fromCollType.getEpoch(),
                               fromCollType.getTimestamp(),
                               Date_t::now(),
                               fromCollType.getUuid(),
                               kShardKeyPattern};
    mockCatalogClient()->setCollectionMetadata(renamedColl, {});

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           fromCollType.getUuid(),
                                                           kToNss,
                                                           boost::none,
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimary */);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 0);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kToNss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
}

}  // namespace
}  // namespace mongo
