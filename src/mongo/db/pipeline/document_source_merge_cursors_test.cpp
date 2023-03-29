/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/query/document_source_merge_cursors.h"

#include <memory>

#include "mongo/bson/oid.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

using ResponseStatus = executor::TaskExecutor::ResponseStatus;

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardId> kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> kTestShardHosts = {HostAndPort("FakeShard1Host", 12345),
                                                  HostAndPort("FakeShard2Host", 12345),
                                                  HostAndPort("FakeShard3Host", 12345)};

const std::string kMergeCursorNsStr{"test.mergeCursors"};
const HostAndPort kTestHost = HostAndPort("localhost:27017"_sd);

const CursorId kExhaustedCursorID = 0;

}  // namespace

class DocumentSourceMergeCursorsTest : public ShardingTestFixture {
public:
    DocumentSourceMergeCursorsTest()
        : _nss(NamespaceString::createNamespaceString_forTest(boost::none, kMergeCursorNsStr)) {
        TimeZoneDatabase::set(getServiceContext(), std::make_unique<TimeZoneDatabase>());
    }

    void setUp() override {
        ShardingTestFixture::setUp();
        setRemote(HostAndPort("ClientHost", 12345));

        _expCtx = makeExpCtx();
        _expCtx->mongoProcessInterface = std::make_shared<StubMongoProcessInterface>(executor());

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        std::vector<ShardType> shards;
        for (size_t i = 0; i < kTestShardIds.size(); i++) {
            ShardType shardType;
            shardType.setName(kTestShardIds[i].toString());
            shardType.setHost(kTestShardHosts[i].toString());

            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHosts[i]));
            targeter->setFindHostReturnValue(kTestShardHosts[i]);

            targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHosts[i]),
                                                   std::move(targeter));
        }

        setupShards(shards);

        CurOp::get(operationContext())->ensureStarted();
    }

    boost::intrusive_ptr<ExpressionContext> getExpCtx() {
        return _expCtx.get();
    }

    NamespaceString getTenantIdNss() const {
        return _nss;
    }

protected:
    virtual boost::intrusive_ptr<ExpressionContext> makeExpCtx() {
        return new ExpressionContext(operationContext(), nullptr, _nss);
    }

    NamespaceString _nss;

private:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectNonArray) {
    auto spec = BSON("$mergeCursors" << 2);
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       17026);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectEmptyArray) {
    auto spec = BSON("$mergeCursors" << BSONArray());
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       17026);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldRejectLegacySerializationFormats) {
    // Formats like this were used in old versions of the server but are no longer supported.
    auto spec = BSON("$mergeCursors" << BSON_ARRAY(BSON("ns" << getTenantIdNss().ns() << "id" << 0LL
                                                             << "host" << kTestHost.toString())));
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       17026);
    spec = BSON("$mergeCursors" << BSONArray());
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       17026);
    spec = BSON("$mergeCursors" << BSON_ARRAY(
                    BSON("ns" << 4 << "id" << 0LL << "host" << kTestHost.toString())));
    ASSERT_THROWS_CODE(DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx()),
                       AssertionException,
                       17026);
}

RemoteCursor makeRemoteCursor(ShardId shardId, HostAndPort host, CursorResponse response) {
    RemoteCursor remoteCursor;
    remoteCursor.setShardId(std::move(shardId));
    remoteCursor.setHostAndPort(std::move(host));
    remoteCursor.setCursorResponse(std::move(response));
    return remoteCursor;
}

void checkSerializedAsyncResultsMergerParams(const AsyncResultsMergerParams& params,
                                             const AsyncResultsMergerParams& serializedParam,
                                             const NamespaceString& testNss) {
    ASSERT_TRUE(params.getSort());
    ASSERT_BSONOBJ_EQ(*params.getSort(), *serializedParam.getSort());
    ASSERT_EQ(params.getCompareWholeSortKey(), serializedParam.getCompareWholeSortKey());
    ASSERT(params.getTailableMode() == serializedParam.getTailableMode());
    ASSERT(params.getBatchSize() == serializedParam.getBatchSize());
    ASSERT_EQ(params.getNss(), serializedParam.getNss());
    ASSERT_EQ(params.getAllowPartialResults(), serializedParam.getAllowPartialResults());
    ASSERT_EQ(serializedParam.getRemotes().size(), 1UL);
    ASSERT(serializedParam.getRemotes()[0].getShardId() == kTestShardIds[0].toString());
    ASSERT(serializedParam.getRemotes()[0].getHostAndPort() == kTestShardHosts[0]);
    ASSERT_EQ(serializedParam.getRemotes()[0].getCursorResponse().getNSS(), testNss);
    ASSERT_EQ(serializedParam.getRemotes()[0].getCursorResponse().getCursorId(),
              kExhaustedCursorID);
    ASSERT(serializedParam.getRemotes()[0].getCursorResponse().getBatch().empty());
}

AsyncResultsMergerParams createAsynchResultsMergerParams(const NamespaceString& nss) {
    AsyncResultsMergerParams params;
    params.setSort(BSON("y" << 1 << "z" << 1));
    params.setNss(nss);
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(nss, kExhaustedCursorID, {})));
    params.setRemotes(std::move(cursors));
    return params;
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldBeAbleToParseSerializedARMParams) {
    AsyncResultsMergerParams params = createAsynchResultsMergerParams(getTenantIdNss());

    auto spec = BSON("$mergeCursors" << params.toBSON());
    auto mergeCursors =
        DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx());
    std::vector<Value> serializationArray;
    mergeCursors->serializeToArray(serializationArray);
    ASSERT_EQ(serializationArray.size(), 1UL);

    // Make sure the serialized version can be parsed into an identical AsyncResultsMergerParams.
    auto newSpec = serializationArray[0].getDocument().toBson();
    ASSERT(newSpec["$mergeCursors"].type() == BSONType::Object);
    auto newParams = AsyncResultsMergerParams::parse(IDLParserContext("$mergeCursors test"),
                                                     newSpec["$mergeCursors"].Obj());
    checkSerializedAsyncResultsMergerParams(params, newParams, getTenantIdNss());

    // Test that the $mergeCursors stage will accept the serialized format of
    // AsyncResultsMergerParams.
    ASSERT(DocumentSourceMergeCursors::createFromBson(newSpec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldReportEOFWithNoCursors) {
    auto expCtx = getExpCtx();
    AsyncResultsMergerParams armParams;
    armParams.setNss(getTenantIdNss());
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, kExhaustedCursorID, {})));
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->ns, kExhaustedCursorID, {})));
    armParams.setRemotes(std::move(cursors));
    auto pipeline = Pipeline::create({}, expCtx);
    auto mergeCursorsStage = DocumentSourceMergeCursors::create(expCtx, std::move(armParams));

    ASSERT_TRUE(mergeCursorsStage->getNext().isEOF());
}

BSONObj cursorResponseObj(const NamespaceString& nss,
                          CursorId cursorId,
                          std::vector<BSONObj> batch) {
    return CursorResponse{nss, cursorId, std::move(batch)}.toBSON(
        CursorResponse::ResponseType::SubsequentResponse);
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldBeAbleToIterateCursorsUntilEOF) {
    auto expCtx = getExpCtx();
    AsyncResultsMergerParams armParams;
    armParams.setNss(getTenantIdNss());
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, 1, {})));
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->ns, 2, {})));
    armParams.setRemotes(std::move(cursors));
    auto pipeline = Pipeline::create({}, expCtx);
    pipeline->addInitialSource(DocumentSourceMergeCursors::create(expCtx, std::move(armParams)));

    // Iterate the $mergeCursors stage asynchronously on a different thread, since it will block
    // waiting for network responses, which we will manually schedule below.
    auto future = launchAsync([&pipeline]() {
        for (int i = 0; i < 5; ++i) {
            ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 1}}));
        }
        ASSERT_FALSE(static_cast<bool>(pipeline->getNext()));
    });


    // Schedule responses to two getMores which keep the cursor open.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(
            expCtx->ns, request.cmdObj["getMore"].Long(), {BSON("x" << 1), BSON("x" << 1)});
    });
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(
            expCtx->ns, request.cmdObj["getMore"].Long(), {BSON("x" << 1), BSON("x" << 1)});
    });

    // Schedule responses to two getMores which report the cursor is exhausted.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(expCtx->ns, kExhaustedCursorID, {});
    });
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(expCtx->ns, kExhaustedCursorID, {BSON("x" << 1)});
    });

    future.default_timed_get();
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldNotKillCursorsIfTheyAreNotOwned) {
    auto expCtx = getExpCtx();
    AsyncResultsMergerParams armParams;
    armParams.setNss(getTenantIdNss());
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, 1, {})));
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->ns, 2, {})));
    armParams.setRemotes(std::move(cursors));
    auto pipeline = Pipeline::create({}, expCtx);
    pipeline->addInitialSource(DocumentSourceMergeCursors::create(expCtx, std::move(armParams)));

    auto mergeCursors =
        checked_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get());
    mergeCursors->dismissCursorOwnership();

    pipeline.reset();  // Delete the pipeline before using it.

    network()->enterNetwork();
    ASSERT_FALSE(network()->hasReadyRequests());
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldKillCursorIfPartiallyIterated) {
    auto expCtx = getExpCtx();
    AsyncResultsMergerParams armParams;
    armParams.setNss(getTenantIdNss());
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, 1, {})));
    armParams.setRemotes(std::move(cursors));
    auto pipeline = Pipeline::create({}, expCtx);
    pipeline->addInitialSource(DocumentSourceMergeCursors::create(expCtx, std::move(armParams)));

    // Iterate the pipeline asynchronously on a different thread, since it will block waiting for
    // network responses, which we will manually schedule below.
    auto future = launchAsync([&pipeline]() {
        ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 1}}));
        pipeline.reset();  // Stop iterating and delete the pipeline.
    });

    // Note we do not use 'kExhaustedCursorID' here, so the cursor is still open.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(expCtx->ns, 1, {BSON("x" << 1), BSON("x" << 1)});
    });

    // Here we're looking for the killCursors request to be scheduled.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["killCursors"]);
        auto cursors = request.cmdObj["cursors"];
        ASSERT_EQ(cursors.type(), BSONType::Array);
        auto cursorsArray = cursors.Array();
        ASSERT_FALSE(cursorsArray.empty());
        auto cursorId = cursorsArray[0].Long();
        ASSERT(cursorId == 1);
        // The ARM doesn't actually inspect the response of the killCursors, so we don't have to put
        // anything except {ok: 1}.
        return BSON("ok" << 1);
    });
    future.default_timed_get();
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldEnforceSortSpecifiedViaARMParams) {
    auto expCtx = getExpCtx();
    auto pipeline = Pipeline::create({}, expCtx);

    // Make a $mergeCursors stage with a sort on "x" and add it to the front of the pipeline.
    AsyncResultsMergerParams armParams;
    armParams.setNss(getTenantIdNss());
    armParams.setSort(BSON("x" << 1));
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->ns, 1, {})));
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->ns, 2, {})));
    armParams.setRemotes(std::move(cursors));
    pipeline->addInitialSource(DocumentSourceMergeCursors::create(expCtx, std::move(armParams)));

    // After optimization we should only have a $mergeCursors stage.
    pipeline->optimizePipeline();
    ASSERT_EQ(pipeline->getSources().size(), 1UL);
    ASSERT_TRUE(dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));

    // Iterate the pipeline asynchronously on a different thread, since it will block waiting for
    // network responses, which we will manually schedule below.
    auto future = launchAsync([&pipeline]() {
        ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 1}}));
        ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 2}}));
        ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 3}}));
        ASSERT_DOCUMENT_EQ(*pipeline->getNext(), (Document{{"x", 4}}));
        ASSERT_FALSE(static_cast<bool>(pipeline->getNext()));
    });

    onCommand([&](const auto& request) {
        return cursorResponseObj(expCtx->ns,
                                 kExhaustedCursorID,
                                 {BSON("x" << 1 << "$sortKey" << BSON_ARRAY(1)),
                                  BSON("x" << 3 << "$sortKey" << BSON_ARRAY(3))});
    });
    onCommand([&](const auto& request) {
        return cursorResponseObj(expCtx->ns,
                                 kExhaustedCursorID,
                                 {BSON("x" << 2 << "$sortKey" << BSON_ARRAY(2)),
                                  BSON("x" << 4 << "$sortKey" << BSON_ARRAY(4))});
    });

    future.default_timed_get();
}

class DocumentSourceMergeCursorsMultiTenancyTest : public DocumentSourceMergeCursorsTest {
public:
    DocumentSourceMergeCursorsMultiTenancyTest()
        : _multitenancyController(
              std::make_unique<RAIIServerParameterControllerForTest>("multitenancySupport", true)) {
        _nss =
            NamespaceString::createNamespaceString_forTest(TenantId(OID::gen()), kMergeCursorNsStr);
    }

protected:
    boost::optional<NamespaceString> getAsyncResultMergerParamsNssFromMergeCursors(
        DocumentSourceMergeCursors* mergeCursors) {
        return mergeCursors->getAsyncResultMergerParamsNss_forTest();
    }

private:
    virtual boost::intrusive_ptr<ExpressionContext> makeExpCtx() override {
        return new ExpressionContext(operationContext(), nullptr, _nss);
    }

    std::unique_ptr<RAIIServerParameterControllerForTest> _multitenancyController;
};

TEST_F(DocumentSourceMergeCursorsMultiTenancyTest, ShouldBeAbleToParseSerializedARMParams) {
    AsyncResultsMergerParams params = createAsynchResultsMergerParams(getTenantIdNss());

    const auto paramsBsonObj = params.toBSON();
    const auto tenantId = *params.getNss().dbName().tenantId();
    ASSERT_EQ(tenantId, getTenantIdNss().tenantId());
    const std::string expectedTenantNsStr = str::stream()
        << tenantId.toString() << "_" << kMergeCursorNsStr;
    auto paramBsonNssStr = paramsBsonObj["nss"].str();
    ASSERT_EQ(paramBsonNssStr, expectedTenantNsStr);
    ASSERT_TRUE(paramBsonNssStr.find('_') != std::string::npos);  // check tenantid does prefix.

    // Deserialize from BSON.
    const auto spec = BSON("$mergeCursors" << paramsBsonObj);
    const auto mergeCursorsTmp =
        DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx());
    const auto mergeCursorsPtr = checked_cast<DocumentSourceMergeCursors*>(mergeCursorsTmp.get());

    // Deserialization check
    auto armpNss = getAsyncResultMergerParamsNssFromMergeCursors(mergeCursorsPtr);
    ASSERT(armpNss);
    ASSERT_EQ(*armpNss, getTenantIdNss());                          // check the namespace
    ASSERT_EQ((*armpNss).tenantId(), getTenantIdNss().tenantId());  // check the nss tenantid

    std::vector<Value> serializationArray;
    mergeCursorsPtr->serializeToArray(serializationArray);
    ASSERT_EQ(serializationArray.size(), 1UL);

    // Make sure the serialized version can be parsed into an identical AsyncResultsMergerParams.
    const auto newSpec = serializationArray[0].getDocument().toBson();
    ASSERT(newSpec["$mergeCursors"].type() == BSONType::Object);
    const auto newParams = AsyncResultsMergerParams::parse(
        IDLParserContext("$mergeCursors test", false, tenantId), newSpec["$mergeCursors"].Obj());

    // Check that the namespace contains the tenantid prefix.
    ASSERT_EQ(newParams.toBSON()["nss"].str(), expectedTenantNsStr);
    ASSERT_EQ(newParams.getNss(), getTenantIdNss());
    checkSerializedAsyncResultsMergerParams(params, newParams, getTenantIdNss());

    // Test that the $mergeCursors stage will accept the serialized format of
    // AsyncResultsMergerParams.
    ASSERT(DocumentSourceMergeCursors::createFromBson(newSpec.firstElement(), getExpCtx()));
}

class DocumentSourceMergeCursorsMultiTenancyAndFeatureFlagTest
    : public DocumentSourceMergeCursorsMultiTenancyTest {
public:
    DocumentSourceMergeCursorsMultiTenancyAndFeatureFlagTest()
        : _featureFlagController(std::make_unique<RAIIServerParameterControllerForTest>(
              "featureFlagRequireTenantID", true)) {}

private:
    std::unique_ptr<RAIIServerParameterControllerForTest> _featureFlagController;
};

TEST_F(DocumentSourceMergeCursorsMultiTenancyAndFeatureFlagTest,
       ShouldBeAbleToParseSerializedARMParams) {
    AsyncResultsMergerParams params = createAsynchResultsMergerParams(getTenantIdNss());

    const auto paramsBsonObj = params.toBSON();
    const auto tenantId = *params.getNss().dbName().tenantId();
    ASSERT_EQ(tenantId, getTenantIdNss().tenantId());
    auto paramBsonNssStr = paramsBsonObj["nss"].str();
    ASSERT_EQ(paramBsonNssStr, kMergeCursorNsStr);
    ASSERT_TRUE(paramBsonNssStr.find('_') == std::string::npos);  // check doesn't tenantid prefix.

    // Deserialize from BSON.
    const auto spec = BSON("$mergeCursors" << paramsBsonObj);
    const auto mergeCursorsTmp =
        DocumentSourceMergeCursors::createFromBson(spec.firstElement(), getExpCtx());
    const auto mergeCursorsPtr = checked_cast<DocumentSourceMergeCursors*>(mergeCursorsTmp.get());

    // Deserialization check.
    auto armpNss = getAsyncResultMergerParamsNssFromMergeCursors(mergeCursorsPtr);
    ASSERT(armpNss);
    ASSERT_EQ(*armpNss, getTenantIdNss());                          // check the namespace
    ASSERT_EQ((*armpNss).tenantId(), getTenantIdNss().tenantId());  // check the nss tenantid

    std::vector<Value> serializationArray;
    mergeCursorsPtr->serializeToArray(serializationArray);
    ASSERT_EQ(serializationArray.size(), 1UL);

    // Make sure the serialized version can be parsed into an identical AsyncResultsMergerParams.
    const auto newSpec = serializationArray[0].getDocument().toBson();
    ASSERT(newSpec["$mergeCursors"].type() == BSONType::Object);
    const auto newParams = AsyncResultsMergerParams::parse(
        IDLParserContext("$mergeCursors test", false, tenantId), newSpec["$mergeCursors"].Obj());

    // Check that the namespace doesn't contain the tenantid prefix.
    ASSERT_EQ(newParams.toBSON()["nss"].str(), kMergeCursorNsStr);
    ASSERT_EQ(newParams.getNss(), getTenantIdNss());
    checkSerializedAsyncResultsMergerParams(params, newParams, getTenantIdNss());

    // Test that the $mergeCursors stage will accept the serialized format of
    // AsyncResultsMergerParams.
    ASSERT(DocumentSourceMergeCursors::createFromBson(newSpec.firstElement(), getExpCtx()));
}
}  // namespace mongo
