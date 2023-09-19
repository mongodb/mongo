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

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <system_error>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/locker_impl.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/query/sharded_agg_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

class ShardsvrProcessInterfaceTest : public ShardedAggTestFixture {
public:
    void setUp() override {
        ShardedAggTestFixture::setUp();
        auto service = expCtx()->opCtx->getServiceContext();
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

    // Need a real locker for storage operations.
    getClient()->swapLockState(std::make_unique<LockerImpl>(expCtx()->opCtx->getServiceContext()));

    const NamespaceString kOutNss =
        NamespaceString::createNamespaceString_forTest("unittests-out", "sharded_agg_test");
    auto outStage = DocumentSourceOut::create(kOutNss, expCtx());

    // Attach a write concern, and make sure it is forwarded below.
    WriteConcernOptions wco{WriteConcernOptions::kMajority,
                            WriteConcernOptions::SyncMode::UNSET,
                            WriteConcernOptions::kNoTimeout};
    expCtx()->opCtx->setWriteConcern(wco);

    expCtx()->mongoProcessInterface = std::make_shared<ShardServerProcessInterface>(executor());
    auto queue = DocumentSourceQueue::create(expCtx());
    outStage->setSource(queue.get());

    auto future = launchAsync([&] { ASSERT_TRUE(outStage->getNext().isEOF()); });

    expectGetDatabase(kOutNss);

    // Testing the collection options are propagated.
    const BSONObj collectionOptions = BSON("validationLevel"
                                           << "moderate");
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

    // Mock the response to $out's "listIndexes" request.
    const BSONObj indexBSON = BSON("_id" << 1);
    const BSONObj listIndexesResponse = BSON("v" << 1 << "key" << indexBSON << "name"
                                                 << "_id_"
                                                 << "ns" << kOutNss.toString_forTest());
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("listIndexes", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ(kOutNss.dbName(), request.dbname);
        ASSERT_EQ(kOutNss.coll(), request.cmdObj.firstElement().valueStringDataSafe());
        return CursorResponse(kTestAggregateNss, CursorId{0}, {listIndexesResponse})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Mock the response to $out's "createCollection" request.
    NamespaceString tempNss;
    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ("create", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ(kOutNss.dbName(), request.dbname);
        ASSERT(request.cmdObj.hasField("writeConcern")) << request.cmdObj;
        ASSERT_EQ("moderate", request.cmdObj["validationLevel"].str());

        tempNss = NamespaceString::createNamespaceString_forTest(
            request.dbname, request.cmdObj.firstElement().valueStringDataSafe());
        return BSON("ok" << 1);
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
        ASSERT_EQ("drop", request.cmdObj.firstElement().fieldNameStringData());
        ASSERT_EQ(tempNss.dbName(), request.dbname);
        ASSERT_EQ(tempNss.coll(), request.cmdObj.firstElement().valueStringData());
        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
