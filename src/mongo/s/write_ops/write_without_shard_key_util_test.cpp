/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/ops/update_request.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/concurrency/locker_mongos_client_observer.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace write_without_shard_key {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const int splitPoint = 50;

class WriteWithoutShardKeyUtilTest : public CatalogCacheTestFixture {
public:
    void setUp() override {
        CatalogCacheTestFixture::setUp();

        // Shard key is a compound shard key: {a:1, b:1}.
        const ShardKeyPattern shardKeyPattern(BSON("a" << 1 << "b" << 1));
        _cm = makeCollectionRoutingInfo(kNss,
                                        shardKeyPattern,
                                        nullptr,
                                        false,
                                        {BSON("a" << splitPoint << "b" << splitPoint)},
                                        {})
                  .cm;
    }

    ChunkManager getChunkManager() const {
        return *_cm;
    }

    OperationContext* getOpCtx() {
        return operationContext();
    }

private:
    boost::optional<ChunkManager> _cm;
};

class UnshardedCollectionTest : public CatalogCacheTestFixture {
protected:
    void setUp() override {
        CatalogCacheTestFixture::setUp();
        setupNShards(2);
    }

    OperationContext* getOpCtx() {
        return operationContext();
    }
};

class ProduceUpsertDocumentTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        auto service = getServiceContext();
        service->registerClientObserver(std::make_unique<LockerMongosClientObserver>());
        _opCtx = makeOperationContext();
    }

    OperationContext* getOpCtx() const {
        return _opCtx.get();
    }

protected:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(WriteWithoutShardKeyUtilTest, WriteQueryContainingFullShardKeyCanTargetSingleDocument) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("a" << 1 << "b" << 1),
                                                     {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, false);

    useTwoPhaseProtocol = write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                                       kNss,
                                                                       false /* isUpdateOrDelete */,
                                                                       false /* isUpsert */,
                                                                       BSON("a" << 1 << "b" << 1),
                                                                       {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryContainingPartialShardKeyCannotTargetSingleDocument) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("a" << 1),
                                                     {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, true);

    useTwoPhaseProtocol = write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                                       kNss,
                                                                       false /* isUpdateOrDelete */,
                                                                       false /* isUpsert */,
                                                                       BSON("a" << 1),
                                                                       {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       UpdateAndDeleteQueryContainingUnderscoreIdCanTargetSingleDocument) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("_id" << 1),
                                                     {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithoutShardKeyOrUnderscoreIdCannotTargetSingleDocument) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, true);

    useTwoPhaseProtocol = write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                                       kNss,
                                                                       false /* isUpdateOrDelete */,
                                                                       false /* isUpsert */,
                                                                       BSON("x" << 1),
                                                                       {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest, FindAndModifyQueryWithOnlyIdMustUseTwoPhaseProtocol) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("_id" << 1),
                                                     {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest, FindAndModifyQueryWithoutShardKeyMustUseTwoPhaseProtocol) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest, QueryWithFeatureFlagDisabledDoesNotUseTwoPhaseProtocol) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", false);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     false /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(UnshardedCollectionTest, UnshardedCollectionDoesNotUseTwoPhaseProtocol) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);
    expectGetDatabase(kNss);

    // Return an empty collection
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    // Return no global indexes
    if (feature_flags::gGlobalIndexesShardingCatalog.isEnabledAndIgnoreFCVUnsafe()) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, kConfigHostAndPort);
            ASSERT_EQ(request.dbname, "config");
            return CursorResponse(CollectionType::ConfigNS, CursorId{0}, {})
                .toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }

    auto cri = *future.default_timed_get();
    ASSERT(!cri.cm.isSharded());

    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("x" << 1),
                                                     {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithFullShardKeyAndCollationWithCollatableTypesUsesTwoPhaseProtocol) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("a"
                                                          << "a"
                                                          << "b"
                                                          << "b"),
                                                     BSON("collation"
                                                          << "lowercase") /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithFullShardKeyAndCollationWithoutCollatableTypesDoesNotUseTwoPhaseProtocol) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("a" << 1 << "b" << 1),
                                                     BSON("collation"
                                                          << "lowercase") /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithOnlyIdAndCollationWithCollatableTypeUsesTwoPhaseProtocol) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("_id"
                                                          << "hello"),
                                                     BSON("collation"
                                                          << "lowercase") /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithOnlyIdAndCollationWithoutCollatableTypeDoesNotUseTwoPhaseProtocol) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("_id" << 1),
                                                     BSON("collation"
                                                          << "lowercase") /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, false);
}

TEST_F(WriteWithoutShardKeyUtilTest, WriteQueryWithOnlyIdAndUpsertUsesTwoPhaseProtocol) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     true /* isUpsert */,
                                                     BSON("_id" << BSON("$eq" << 1)),
                                                     {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryContainingPartialShardKeyAndIdPerformingAnUpsertUsesTwoPhaseProtocol) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     true /* isUpsert */,
                                                     BSON("a" << 1 << "_id" << BSON("$eq" << 1)),
                                                     {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(WriteWithoutShardKeyUtilTest,
       WriteQueryWithOnlyIdThatIsNotADirectEqualityUsesTwoPhaseProtocol) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagUpdateOneWithoutShardKey", true);
    auto useTwoPhaseProtocol =
        write_without_shard_key::useTwoPhaseProtocol(getOpCtx(),
                                                     kNss,
                                                     true /* isUpdateOrDelete */,
                                                     false /* isUpsert */,
                                                     BSON("_id" << BSON("$gt" << 1)),
                                                     {} /* collation */);
    ASSERT_EQ(useTwoPhaseProtocol, true);
}

TEST_F(ProduceUpsertDocumentTest, produceUpsertDocumentUsingReplacementUpdate) {
    write_ops::UpdateOpEntry entry;
    entry.setQ(BSON("_id" << 3));
    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON("x" << 2)));

    write_ops::UpdateCommandRequest updateCommandRequest(kNss);
    updateCommandRequest.setUpdates({entry});
    UpdateRequest updateRequest(updateCommandRequest.getUpdates().front());

    auto doc = write_without_shard_key::generateUpsertDocument(getOpCtx(), updateRequest);
    ASSERT_BSONOBJ_EQ(doc, fromjson("{ _id: 3, x: 2 }"));
}

TEST_F(ProduceUpsertDocumentTest, produceUpsertDocumentUsingLetConstantAndPipelineUpdate) {
    write_ops::UpdateOpEntry entry;
    entry.setQ(BSON("_id" << 4 << "x" << 3));

    std::vector<BSONObj> pipelineUpdate;
    pipelineUpdate.push_back(fromjson("{$set: {'x': '$$constOne'}}"));
    pipelineUpdate.push_back(fromjson("{$set: {'y': 3}}"));
    entry.setU(pipelineUpdate);

    BSONObj constants = fromjson("{constOne: 'foo'}");
    entry.setC(constants);

    write_ops::UpdateCommandRequest updateCommandRequest(kNss);
    updateCommandRequest.setUpdates({entry});
    UpdateRequest updateRequest(updateCommandRequest.getUpdates().front());

    auto doc = write_without_shard_key::generateUpsertDocument(getOpCtx(), updateRequest);
    ASSERT_BSONOBJ_EQ(doc, fromjson("{ _id: 4, x: 'foo', y: 3 }"));
}

TEST_F(ProduceUpsertDocumentTest, produceUpsertDocumentUsingArrayFilterAndModificationUpdate) {
    write_ops::UpdateOpEntry entry;
    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("a" << 90));
    entry.setQ(BSON("_id" << 4 << "x" << arrayBuilder.arr()));
    entry.setU(
        write_ops::UpdateModification::parseFromClassicUpdate(fromjson("{$inc: {'x.$[b].a': 3}}")));

    auto arrayFilter = std::vector<BSONObj>{fromjson("{'b.a': {$gt: 85}}")};
    entry.setArrayFilters(arrayFilter);

    write_ops::UpdateCommandRequest updateCommandRequest(kNss);
    updateCommandRequest.setUpdates({entry});
    UpdateRequest updateRequest(updateCommandRequest.getUpdates().front());

    auto doc = write_without_shard_key::generateUpsertDocument(getOpCtx(), updateRequest);
    ASSERT_BSONOBJ_EQ(doc, fromjson("{ _id: 4, x: [ { a: 93 } ] }"));
}

TEST_F(ProduceUpsertDocumentTest, produceUpsertDocumentUsingCollation) {
    write_ops::UpdateOpEntry entry;
    BSONArrayBuilder arrayBuilder;
    arrayBuilder.append(BSON("a"
                             << "BAR"));
    arrayBuilder.append(BSON("a"
                             << "bar"));
    arrayBuilder.append(BSON("a"
                             << "foo"));
    entry.setQ(BSON("_id" << 4 << "x" << arrayBuilder.arr()));
    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
        fromjson("{$set: {'x.$[b].a': 'FOO'}}")));

    auto arrayFilter = std::vector<BSONObj>{fromjson("{'b.a': {$eq: 'bar'}}")};
    entry.setArrayFilters(arrayFilter);
    entry.setCollation(fromjson("{locale: 'en_US', strength: 2}"));

    write_ops::UpdateCommandRequest updateCommandRequest(kNss);
    updateCommandRequest.setUpdates({entry});
    UpdateRequest updateRequest(updateCommandRequest.getUpdates().front());

    auto doc = write_without_shard_key::generateUpsertDocument(getOpCtx(), updateRequest);
    ASSERT_BSONOBJ_EQ(doc, fromjson("{ _id: 4, x: [ { a: 'FOO' }, { a: 'FOO' }, { a: 'foo' } ] }"));
}

}  // namespace
}  // namespace write_without_shard_key
}  // namespace mongo
