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

#include "mongo/db/index_builds/resumable_index_builds_common.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

IndexStateInfo makeIndexStateInfo(boost::optional<RecordId> lastSpilled) {
    IndexStateInfo info;
    if (lastSpilled) {
        info.setLastSpilledRecordId(*lastSpilled);
    }
    return info;
}

TEST(minLastSpilledRecordIdTest, EmptyVectorReturnsNone) {
    std::vector<IndexStateInfo> indexes;
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), boost::none);
}

TEST(minLastSpilledRecordIdTest, SingleIndexMissingReturnsNone) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(boost::none)};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), boost::none);
}

TEST(minLastSpilledRecordIdTest, SingleIndexPresentReturnsItsValue) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(RecordId{42})};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), RecordId{42});
}

TEST(minLastSpilledRecordIdTest, MultipleIndexesAllPresentReturnsMinimum) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(RecordId{20}),
                                        makeIndexStateInfo(RecordId{5}),
                                        makeIndexStateInfo(RecordId{12})};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), RecordId{5});
}

TEST(minLastSpilledRecordIdTest, DuplicateMinimaReturnsSharedMin) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(RecordId{7}),
                                        makeIndexStateInfo(RecordId{20}),
                                        makeIndexStateInfo(RecordId{7})};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), RecordId{7});
}

TEST(minLastSpilledRecordIdTest, FirstIndexMissingReturnsNone) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(boost::none),
                                        makeIndexStateInfo(RecordId{5}),
                                        makeIndexStateInfo(RecordId{12})};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), boost::none);
}

TEST(minLastSpilledRecordIdTest, MiddleIndexMissingReturnsNone) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(RecordId{20}),
                                        makeIndexStateInfo(boost::none),
                                        makeIndexStateInfo(RecordId{12})};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), boost::none);
}

TEST(minLastSpilledRecordIdTest, LastIndexMissingReturnsNone) {
    std::vector<IndexStateInfo> indexes{makeIndexStateInfo(RecordId{20}),
                                        makeIndexStateInfo(RecordId{5}),
                                        makeIndexStateInfo(boost::none)};
    EXPECT_EQ(index_builds::minLastSpilledRecordId(indexes), boost::none);
}

class ReadAndParseResumeIndexInfoTest : public CatalogTestFixture {
protected:
    // Creates the internal record store at `ident` and inserts `records` in order. An empty
    // `records` vector creates the ident with no records.
    void writeRecords(const std::string& ident, const std::vector<BSONObj>& records) {
        auto opCtx = operationContext();
        Lock::GlobalLock lk(opCtx, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        auto rs = getServiceContext()->getStorageEngine()->makeInternalRecordStore(
            opCtx, ident, KeyFormat::Long);
        for (auto&& obj : records) {
            ASSERT_OK(rs->insertRecord(opCtx,
                                       *shard_role_details::getRecoveryUnit(opCtx),
                                       obj.objdata(),
                                       obj.objsize(),
                                       Timestamp{})
                          .getStatus());
        }
        wuow.commit();
    }

    boost::optional<ResumeIndexInfo> read(const std::string& ident) {
        // Read with a fresh snapshot so the cursor sees any records just written.
        shard_role_details::getRecoveryUnit(operationContext())->abandonSnapshot();
        return index_builds::readAndParseResumeIndexInfo(
            getServiceContext()->getStorageEngine(), operationContext(), ident);
    }

    static BSONObj makeMetadata(const UUID& buildUUID,
                                IndexBuildPhaseEnum phase,
                                const UUID& collectionUUID) {
        IndexBuildMetadata metadata;
        metadata.setBuildUUID(buildUUID);
        metadata.setPhase(phase);
        metadata.setCollectionUUID(collectionUUID);
        return metadata.toBSON();
    }

    static IndexStateInfo makeIndexState(StringData sideWritesTable,
                                         boost::optional<RecordId> lastSpilled) {
        IndexStateInfo info;
        info.setSpec(BSON("v" << 2 << "key" << BSON("a" << 1) << "name" << "a_1"));
        info.setIsMultikey(false);
        info.setMultikeyPaths({});
        info.setSideWritesTable(sideWritesTable);
        if (lastSpilled) {
            info.setLastSpilledRecordId(*lastSpilled);
        }
        return info;
    }

    static BSONObj makeSingleRecord(const UUID& buildUUID,
                                    IndexBuildPhaseEnum phase,
                                    const UUID& collectionUUID,
                                    std::vector<IndexStateInfo> indexes,
                                    boost::optional<RecordId> collectionScanPosition) {
        ResumeIndexInfo info;
        info.setBuildUUID(buildUUID);
        info.setPhase(phase);
        info.setCollectionUUID(collectionUUID);
        if (collectionScanPosition) {
            info.setCollectionScanPosition(*collectionScanPosition);
        }
        info.setIndexes(std::move(indexes));
        return info.toBSON();
    }
};

TEST_F(ReadAndParseResumeIndexInfoTest, MissingIdent) {
    EXPECT_EQ(read(ident::generateNewIndexBuildIdent(UUID::gen())), boost::none);
}

TEST_F(ReadAndParseResumeIndexInfoTest, EmptyTable) {
    auto ident = ident::generateNewIndexBuildIdent(UUID::gen());
    writeRecords(ident, {});
    EXPECT_EQ(read(ident), boost::none);
}

TEST_F(ReadAndParseResumeIndexInfoTest, SingleRecordLayout) {
    auto buildUUID = UUID::gen();
    auto collectionUUID = UUID::gen();
    auto ident = ident::generateNewIndexBuildIdent(buildUUID);

    writeRecords(ident,
                 {makeSingleRecord(buildUUID,
                                   IndexBuildPhaseEnum::kCollectionScan,
                                   collectionUUID,
                                   {makeIndexState("sideWrites-0", RecordId{10}),
                                    makeIndexState("sideWrites-1", RecordId{20})},
                                   RecordId{99})});

    auto resumeInfo = read(ident);
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(resumeInfo->getBuildUUID(), buildUUID);
    EXPECT_EQ(resumeInfo->getPhase(), IndexBuildPhaseEnum::kCollectionScan);
    EXPECT_EQ(resumeInfo->getCollectionUUID(), collectionUUID);
    ASSERT_TRUE(resumeInfo->getCollectionScanPosition());
    EXPECT_EQ(*resumeInfo->getCollectionScanPosition(), RecordId{99});
    ASSERT_EQ(resumeInfo->getIndexes().size(), 2);
    EXPECT_EQ(resumeInfo->getIndexes()[0].getSideWritesTable(), "sideWrites-0");
    EXPECT_EQ(*resumeInfo->getIndexes()[0].getLastSpilledRecordId(), RecordId{10});
    EXPECT_EQ(resumeInfo->getIndexes()[1].getSideWritesTable(), "sideWrites-1");
    EXPECT_EQ(*resumeInfo->getIndexes()[1].getLastSpilledRecordId(), RecordId{20});
}

TEST_F(ReadAndParseResumeIndexInfoTest, MultiRecordLayoutWithTwoRecords) {
    auto buildUUID = UUID::gen();
    auto collectionUUID = UUID::gen();
    auto ident = ident::generateNewIndexBuildIdent(buildUUID);

    writeRecords(ident,
                 {makeMetadata(buildUUID, IndexBuildPhaseEnum::kCollectionScan, collectionUUID),
                  makeIndexState("sideWrites-0", RecordId{5}).toBSON()});

    auto resumeInfo = read(ident);
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(resumeInfo->getBuildUUID(), buildUUID);
    EXPECT_EQ(resumeInfo->getPhase(), IndexBuildPhaseEnum::kCollectionScan);
    EXPECT_EQ(resumeInfo->getCollectionUUID(), collectionUUID);
    ASSERT_EQ(resumeInfo->getIndexes().size(), 1U);
    EXPECT_EQ(resumeInfo->getIndexes()[0].getSideWritesTable(), "sideWrites-0");
    EXPECT_EQ(*resumeInfo->getIndexes()[0].getLastSpilledRecordId(), RecordId{5});
}

TEST_F(ReadAndParseResumeIndexInfoTest, MultiRecordLayoutWithSeveralRecords) {
    auto buildUUID = UUID::gen();
    auto collectionUUID = UUID::gen();
    auto ident = ident::generateNewIndexBuildIdent(buildUUID);

    writeRecords(ident,
                 {makeMetadata(buildUUID, IndexBuildPhaseEnum::kBulkLoad, collectionUUID),
                  makeIndexState("sideWrites-0", RecordId{10}).toBSON(),
                  makeIndexState("sideWrites-1", RecordId{20}).toBSON(),
                  makeIndexState("sideWrites-2", boost::none).toBSON()});

    auto resumeInfo = read(ident);
    ASSERT_TRUE(resumeInfo);
    EXPECT_EQ(resumeInfo->getBuildUUID(), buildUUID);
    EXPECT_EQ(resumeInfo->getPhase(), IndexBuildPhaseEnum::kBulkLoad);
    EXPECT_EQ(resumeInfo->getCollectionUUID(), collectionUUID);
    ASSERT_EQ(resumeInfo->getIndexes().size(), 3U);
    EXPECT_EQ(resumeInfo->getIndexes()[0].getSideWritesTable(), "sideWrites-0");
    EXPECT_EQ(*resumeInfo->getIndexes()[0].getLastSpilledRecordId(), RecordId{10});
    EXPECT_EQ(resumeInfo->getIndexes()[1].getSideWritesTable(), "sideWrites-1");
    EXPECT_EQ(*resumeInfo->getIndexes()[1].getLastSpilledRecordId(), RecordId{20});
    EXPECT_EQ(resumeInfo->getIndexes()[2].getSideWritesTable(), "sideWrites-2");
    EXPECT_FALSE(resumeInfo->getIndexes()[2].getLastSpilledRecordId());
}

TEST_F(ReadAndParseResumeIndexInfoTest, InvalidSingleRecord) {
    auto ident = ident::generateNewIndexBuildIdent(UUID::gen());

    // A single record that is not a valid ResumeIndexInfo: returns none.
    writeRecords(ident, {BSON("foo" << BSON("bar" << "baz"))});

    EXPECT_EQ(read(ident), boost::none);
}

TEST_F(ReadAndParseResumeIndexInfoTest, InvalidIndexStateInMultiRecordLayout) {
    auto buildUUID = UUID::gen();
    auto collectionUUID = UUID::gen();
    auto ident = ident::generateNewIndexBuildIdent(buildUUID);

    // A valid metadata record followed by a garbage IndexStateInfo record: returns none.
    writeRecords(ident,
                 {makeMetadata(buildUUID, IndexBuildPhaseEnum::kBulkLoad, collectionUUID),
                  BSON("foo" << BSON("bar" << "baz"))});

    EXPECT_EQ(read(ident), boost::none);
}

}  // namespace
}  // namespace mongo
