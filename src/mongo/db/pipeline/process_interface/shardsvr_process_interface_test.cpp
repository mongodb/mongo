/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"

#include "mongo/base/shim.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/queue_stage.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/query/exec/sharded_agg_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

std::shared_ptr<MongoProcessInterface> MongoProcessInterfaceCreateImpl(OperationContext* opCtx) {
    return std::make_shared<ShardServerProcessInterface>(
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor());
}

auto mongoProcessInterfaceCreateRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    MongoProcessInterface::create, MongoProcessInterfaceCreateImpl);

class ShardsvrProcessInterfaceTest : public ShardedAggTestFixture {
public:
    void setUp() override {
        ShardedAggTestFixture::setUp();
        auto service = expCtx()->getOperationContext()->getServiceContext();
        repl::ReplSettings settings;

        settings.setReplSetString("lookupTestSet/node1:12345");

        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceMock>());
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service, settings);

        // Ensure that we are primary.
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));
    }
};

TEST_F(ShardsvrProcessInterfaceTest, TestInsert) {
    setupNShards(2);

    const NamespaceString kOutNss =
        NamespaceString::createNamespaceString_forTest("unittests-out", "sharded_agg_test");
    auto outStage = DocumentSourceOut::create(kOutNss, expCtx());
    auto stage = exec::agg::buildStage(outStage);

    // Attach a write concern, and make sure it is forwarded below.
    WriteConcernOptions wco{WriteConcernOptions::kMajority,
                            WriteConcernOptions::SyncMode::UNSET,
                            WriteConcernOptions::kNoTimeout};
    expCtx()->getOperationContext()->setWriteConcern(wco);

    expCtx()->setMongoProcessInterface(std::make_shared<ShardServerProcessInterface>(executor()));
    auto queueStage = exec::agg::buildStage(DocumentSourceQueue::create(expCtx()));
    stage->setSource(queueStage.get());

    auto future = launchAsync([&] { ASSERT_TRUE(stage->getNext().isEOF()); });

    expectGetDatabase(kOutNss);

    // Testing the collection options are propagated.
    const BSONObj collectionOptions = BSON("validationLevel" << "moderate");
    const BSONObj listCollectionsResponse = BSON("name" << kOutNss.coll() << "type"
                                                        << "collection"
                                                        << "options" << collectionOptions);

    // Mock the response to $out's "listCollections" request.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("listCollections", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ(kOutNss.dbName(), request.dbname);
        ASSERT_EQ(kOutNss.coll(), request.cmdObj["filter"]["name"].valueStringData());
        return CursorResponse(kTestAggregateNss, CursorId{0}, {listCollectionsResponse})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Mock the response to $out's "aggregate" request to config server, that is a part of
    // getIndexSpecs.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("aggregate", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ("collections", request.cmdObj.firstElement().valueStringDataSafe());
        // Response is empty for the unsharded untracked collection.
        return CursorResponse(kTestAggregateNss, CursorId{0}, {})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Mock the response to $out's "listIndexes" request.
    const BSONObj indexBSON = BSON("_id" << 1);
    const BSONObj listIndexesResponse =
        BSON("v" << 1 << "key" << indexBSON << "name" << IndexConstants::kIdIndexName << "ns"
                 << kOutNss.toString_forTest());
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("listIndexes", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ(kOutNss.dbName(), request.dbname);
        ASSERT_EQ(kOutNss.coll(), request.cmdObj.firstElement().valueStringDataSafe());
        return CursorResponse(kTestAggregateNss, CursorId{0}, {listIndexesResponse})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Mock the response to $out's "_shardsvrCreateCollection" request.
    NamespaceString tempNss;
    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("_shardsvrCreateCollection", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ(kOutNss.dbName(), request.dbname);
        ASSERT(request.cmdObj.hasField("writeConcern")) << request.cmdObj;
        ASSERT_EQ("moderate", request.cmdObj["validationLevel"].str());
        ASSERT_EQ(true, request.cmdObj["unsplittable"].boolean());

        tempNss = NamespaceString::createNamespaceString_forTest(
            request.dbname, request.cmdObj.firstElement().valueStringDataSafe());
        CreateCollectionResponse res;
        res.setCollectionVersion(ShardVersion{});
        return res.toBSON();
    });

    // Mock the response to $out's "listCollections" request to get the uuid of the temp collection.
    auto uuid = UUID::gen();
    const BSONObj collectionInfo = BSON("readOnly" << false << "uuid" << uuid);
    const BSONObj listCollectionsGetUUIDResponse =
        BSON("name" << tempNss.coll() << "type"
                    << "collection"
                    << "options" << collectionOptions << "info" << collectionInfo);
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("listCollections", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ(kOutNss.dbName(), request.dbname);
        ASSERT_EQ(tempNss.coll(), request.cmdObj["filter"]["name"].valueStringData());
        return CursorResponse(kTestAggregateNss, CursorId{0}, {listCollectionsGetUUIDResponse})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Mock the response to $out's "aggregate" request to config server, that is a part of
    // createIndexes.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("aggregate", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ("collections", request.cmdObj.firstElement().valueStringDataSafe());
        // Response is empty for the unsharded untracked collection.
        return CursorResponse(kTestAggregateNss, CursorId{0}, {})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Mock the response to $out's "createIndexes" request.
    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("createIndexes", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ(tempNss.dbName(), request.dbname);
        ASSERT_EQ(tempNss.coll(), request.cmdObj.firstElement().valueStringData());
        ASSERT(request.cmdObj.hasField("writeConcern")) << request.cmdObj;

        ASSERT(request.cmdObj.hasField("indexes"));
        const std::vector<BSONElement>& indexArray = request.cmdObj["indexes"].Array();
        ASSERT_EQ(1, indexArray.size());
        ASSERT_BSONOBJ_EQ(listIndexesResponse, indexArray.at(0).Obj());
        return BSON("ok" << 1);
    });

    // Mock the response to $out's "listCollections" request to get the uuid of the temp collection.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("listCollections", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ(kOutNss.dbName(), request.dbname);
        ASSERT_EQ(tempNss.coll(), request.cmdObj["filter"]["name"].valueStringData());
        return CursorResponse(kTestAggregateNss, CursorId{0}, {listCollectionsGetUUIDResponse})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Mock the response to $out's "internalRenameIfOptionsAndIndexesMatch" request.
    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("internalRenameIfOptionsAndIndexesMatch",
                  request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ(tempNss.toString_forTest(), request.cmdObj["from"].String());
        ASSERT_EQ(kOutNss.toString_forTest(), request.cmdObj["to"].String());
        ASSERT(request.cmdObj.hasField("writeConcern")) << request.cmdObj;
        return BSON("ok" << 1);
    });

    // Mock the response to the drop of the temporary collection.
    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("_shardsvrDropCollection", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ(tempNss.dbName(), request.dbname);
        ASSERT_EQ(tempNss.coll(), request.cmdObj.firstElement().valueStringData());
        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
