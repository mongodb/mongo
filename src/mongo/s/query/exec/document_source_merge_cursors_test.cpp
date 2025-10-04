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

// IWYU pragma: no_include "cxxabi.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

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
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        _expCtx = makeExpCtx();
        _expCtx->setMongoProcessInterface(std::make_shared<StubMongoProcessInterface>(executor()));

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
        return ExpressionContextBuilder{}.opCtx(operationContext()).ns(_nss).build();
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
    auto spec =
        BSON("$mergeCursors" << BSON_ARRAY(BSON("ns" << getTenantIdNss().ns_forTest() << "id" << 0LL
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
    ASSERT(newSpec["$mergeCursors"].type() == BSONType::object);
    auto newParams = AsyncResultsMergerParams::parse(newSpec["$mergeCursors"].Obj(),
                                                     IDLParserContext("$mergeCursors test"));
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
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[0],
                         kTestShardHosts[0],
                         CursorResponse(expCtx->getNamespaceString(), kExhaustedCursorID, {})));
    cursors.emplace_back(
        makeRemoteCursor(kTestShardIds[1],
                         kTestShardHosts[1],
                         CursorResponse(expCtx->getNamespaceString(), kExhaustedCursorID, {})));
    armParams.setRemotes(std::move(cursors));
    auto pipeline = Pipeline::create({}, expCtx);
    auto source = DocumentSourceMergeCursors::create(expCtx, std::move(armParams));
    auto stage = exec::agg::buildStage(source);
    ASSERT_TRUE(stage->getNext().isEOF());
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
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->getNamespaceString(), 1, {})));
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->getNamespaceString(), 2, {})));
    armParams.setRemotes(std::move(cursors));
    auto pipeline = Pipeline::create({}, expCtx);
    pipeline->addInitialSource(DocumentSourceMergeCursors::create(expCtx, std::move(armParams)));
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    // Iterate the $mergeCursors stage asynchronously on a different thread, since it will block
    // waiting for network responses, which we will manually schedule below.
    auto future = launchAsync([&execPipeline]() {
        for (int i = 0; i < 5; ++i) {
            ASSERT_DOCUMENT_EQ(*execPipeline->getNext(), (Document{{"x", 1}}));
        }
        ASSERT_FALSE(static_cast<bool>(execPipeline->getNext()));
    });


    // Schedule responses to two getMores which keep the cursor open.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(expCtx->getNamespaceString(),
                                 request.cmdObj["getMore"].Long(),
                                 {BSON("x" << 1), BSON("x" << 1)});
    });
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(expCtx->getNamespaceString(),
                                 request.cmdObj["getMore"].Long(),
                                 {BSON("x" << 1), BSON("x" << 1)});
    });

    // Schedule responses to two getMores which report the cursor is exhausted.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(expCtx->getNamespaceString(), kExhaustedCursorID, {});
    });
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(
            expCtx->getNamespaceString(), kExhaustedCursorID, {BSON("x" << 1)});
    });

    future.default_timed_get();
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldNotKillCursorsIfTheyAreNotOwned) {
    auto expCtx = getExpCtx();
    AsyncResultsMergerParams armParams;
    armParams.setNss(getTenantIdNss());
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->getNamespaceString(), 1, {})));
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->getNamespaceString(), 2, {})));
    armParams.setRemotes(std::move(cursors));
    auto pipeline = Pipeline::create({}, expCtx);
    pipeline->addInitialSource(DocumentSourceMergeCursors::create(expCtx, std::move(armParams)));

    auto mergeCursors =
        checked_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get());
    mergeCursors->dismissCursorOwnership();

    pipeline.reset();  // Delete the pipeline before using it.

    ASSERT_FALSE(executor::NetworkInterfaceMock::InNetworkGuard(network())->hasReadyRequests());
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldKillCursorIfPartiallyIterated) {
    auto expCtx = getExpCtx();
    AsyncResultsMergerParams armParams;
    armParams.setNss(getTenantIdNss());
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->getNamespaceString(), 1, {})));
    armParams.setRemotes(std::move(cursors));
    auto pipeline = Pipeline::create({}, expCtx);
    pipeline->addInitialSource(DocumentSourceMergeCursors::create(expCtx, std::move(armParams)));
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    // Iterate the pipeline asynchronously on a different thread, since it will block waiting for
    // network responses, which we will manually schedule below.
    auto future = launchAsync([&]() {
        ASSERT_DOCUMENT_EQ(*execPipeline->getNext(), (Document{{"x", 1}}));
        execPipeline.reset();  // Stop iterating and delete the pipeline.
        pipeline.reset();
    });

    // Note we do not use 'kExhaustedCursorID' here, so the cursor is still open.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["getMore"]);
        return cursorResponseObj(expCtx->getNamespaceString(), 1, {BSON("x" << 1), BSON("x" << 1)});
    });

    // Here we're looking for the killCursors request to be scheduled.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["killCursors"]);
        auto cursors = request.cmdObj["cursors"];
        ASSERT_EQ(cursors.type(), BSONType::array);
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
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->getNamespaceString(), 1, {})));
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->getNamespaceString(), 2, {})));
    armParams.setRemotes(std::move(cursors));
    pipeline->addInitialSource(DocumentSourceMergeCursors::create(expCtx, std::move(armParams)));

    // After optimization we should only have a $mergeCursors stage.
    pipeline->optimizePipeline();
    ASSERT_EQ(pipeline->getSources().size(), 1UL);
    ASSERT_TRUE(dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));

    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    // Iterate the pipeline asynchronously on a different thread, since it will block waiting for
    // network responses, which we will manually schedule below.
    auto future = launchAsync([&execPipeline]() {
        ASSERT_DOCUMENT_EQ(*execPipeline->getNext(), (Document{{"x", 1}}));
        ASSERT_DOCUMENT_EQ(*execPipeline->getNext(), (Document{{"x", 2}}));
        ASSERT_DOCUMENT_EQ(*execPipeline->getNext(), (Document{{"x", 3}}));
        ASSERT_DOCUMENT_EQ(*execPipeline->getNext(), (Document{{"x", 4}}));
        ASSERT_FALSE(static_cast<bool>(execPipeline->getNext()));
    });

    onCommand([&](const auto& request) {
        return cursorResponseObj(expCtx->getNamespaceString(),
                                 kExhaustedCursorID,
                                 {BSON("x" << 1 << "$sortKey" << BSON_ARRAY(1)),
                                  BSON("x" << 3 << "$sortKey" << BSON_ARRAY(3))});
    });
    onCommand([&](const auto& request) {
        return cursorResponseObj(expCtx->getNamespaceString(),
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
    boost::intrusive_ptr<ExpressionContext> makeExpCtx() override {
        return ExpressionContextBuilder{}.opCtx(operationContext()).ns(_nss).build();
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
    ASSERT(newSpec["$mergeCursors"].type() == BSONType::object);
    const auto vts = auth::ValidatedTenancyScopeFactory::create(
        tenantId,
        auth::ValidatedTenancyScope::TenantProtocol::kDefault,
        auth::ValidatedTenancyScopeFactory::TenantForTestingTag{});
    const auto newParams = AsyncResultsMergerParams::parse(
        newSpec["$mergeCursors"].Obj(),
        IDLParserContext(
            "$mergeCursors test", vts, tenantId, SerializationContext::stateDefault()));

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
    ASSERT(newSpec["$mergeCursors"].type() == BSONType::object);
    const auto vts = auth::ValidatedTenancyScopeFactory::create(
        tenantId,
        auth::ValidatedTenancyScope::TenantProtocol::kDefault,
        auth::ValidatedTenancyScopeFactory::TenantForTestingTag{});
    const auto newParams = AsyncResultsMergerParams::parse(
        newSpec["$mergeCursors"].Obj(),
        IDLParserContext(
            "$mergeCursors test", vts, tenantId, SerializationContext::stateDefault()));

    // Check that the namespace doesn't contain the tenantid prefix.
    ASSERT_EQ(newParams.toBSON()["nss"].str(), kMergeCursorNsStr);
    ASSERT_EQ(newParams.getNss(), getTenantIdNss());
    checkSerializedAsyncResultsMergerParams(params, newParams, getTenantIdNss());

    // Test that the $mergeCursors stage will accept the serialized format of
    // AsyncResultsMergerParams.
    ASSERT(DocumentSourceMergeCursors::createFromBson(newSpec.firstElement(), getExpCtx()));
}
using DocumentSourceMergeCursorsShapeTest = AggregationContextFixture;
TEST_F(DocumentSourceMergeCursorsShapeTest, QueryShape) {
    auto expCtx = getExpCtx();
    AsyncResultsMergerParams armParams;
    armParams.setNss(
        NamespaceString::createNamespaceString_forTest(boost::none, kMergeCursorNsStr));
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(expCtx->getNamespaceString(), 1, {})));
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[1], kTestShardHosts[1], CursorResponse(expCtx->getNamespaceString(), 2, {})));
    armParams.setRemotes(std::move(cursors));
    auto stage = DocumentSourceMergeCursors::create(expCtx, std::move(armParams));

    // There is no need for closing remote cursors within this unit-test.
    stage->dismissCursorOwnership();

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$mergeCursors": {
                "compareWholeSortKey": "?bool",
                "remotes": [
                    {
                        "shardId": "HASH<FakeShard1>",
                        "hostAndPort": "HASH<FakeShard1Host:12345>",
                        "cursorResponse": "?object"
                    },
                    {
                        "shardId": "HASH<FakeShard2>",
                        "hostAndPort": "HASH<FakeShard2Host:12345>",
                        "cursorResponse": "?object"
                    }
                ],
                "nss": "HASH<test.mergeCursors>",
                "allowPartialResults": false,
                "recordRemoteOpWaitTime": false,
                "requestQueryStatsFromRemotes": false
            }
        })",
        redact(*stage));
}
}  // namespace mongo
