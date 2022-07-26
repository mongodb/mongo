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
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
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

const NamespaceString kTestNss = NamespaceString(boost::none, "test.mergeCursors"_sd);
const HostAndPort kTestHost = HostAndPort("localhost:27017"_sd);

const CursorId kExhaustedCursorID = 0;

class DocumentSourceMergeCursorsTest : public ShardingTestFixture {
public:
    DocumentSourceMergeCursorsTest() {
        TimeZoneDatabase::set(getServiceContext(), std::make_unique<TimeZoneDatabase>());
    }

    void setUp() override {
        ShardingTestFixture::setUp();
        setRemote(HostAndPort("ClientHost", 12345));

        _expCtx = new ExpressionContext(operationContext(), nullptr, kTestNss);
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
    auto spec = BSON("$mergeCursors" << BSON_ARRAY(BSON("ns" << kTestNss.ns() << "id" << 0LL
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

TEST_F(DocumentSourceMergeCursorsTest, ShouldBeAbleToParseSerializedARMParams) {
    AsyncResultsMergerParams params;
    params.setSort(BSON("y" << 1 << "z" << 1));
    params.setNss(kTestNss);
    std::vector<RemoteCursor> cursors;
    cursors.emplace_back(makeRemoteCursor(
        kTestShardIds[0], kTestShardHosts[0], CursorResponse(kTestNss, kExhaustedCursorID, {})));
    params.setRemotes(std::move(cursors));
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
    ASSERT_TRUE(params.getSort());
    ASSERT_BSONOBJ_EQ(*params.getSort(), *newParams.getSort());
    ASSERT_EQ(params.getCompareWholeSortKey(), newParams.getCompareWholeSortKey());
    ASSERT(params.getTailableMode() == newParams.getTailableMode());
    ASSERT(params.getBatchSize() == newParams.getBatchSize());
    ASSERT_EQ(params.getNss(), newParams.getNss());
    ASSERT_EQ(params.getAllowPartialResults(), newParams.getAllowPartialResults());
    ASSERT_EQ(newParams.getRemotes().size(), 1UL);
    ASSERT(newParams.getRemotes()[0].getShardId() == kTestShardIds[0].toString());
    ASSERT(newParams.getRemotes()[0].getHostAndPort() == kTestShardHosts[0]);
    ASSERT_EQ(newParams.getRemotes()[0].getCursorResponse().getNSS(), kTestNss);
    ASSERT_EQ(newParams.getRemotes()[0].getCursorResponse().getCursorId(), kExhaustedCursorID);
    ASSERT(newParams.getRemotes()[0].getCursorResponse().getBatch().empty());

    // Test that the $mergeCursors stage will accept the serialized format of
    // AsyncResultsMergerParams.
    ASSERT(DocumentSourceMergeCursors::createFromBson(newSpec.firstElement(), getExpCtx()));
}

TEST_F(DocumentSourceMergeCursorsTest, ShouldReportEOFWithNoCursors) {
    auto expCtx = getExpCtx();
    AsyncResultsMergerParams armParams;
    armParams.setNss(kTestNss);
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
    armParams.setNss(kTestNss);
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
    armParams.setNss(kTestNss);
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
    armParams.setNss(kTestNss);
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
    armParams.setNss(kTestNss);
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
}  // namespace
}  // namespace mongo
