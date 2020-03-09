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

#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"
#include "mongo/s/query/sharded_agg_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// Use this new name to register these tests under their own unit test suite.
using ShardedProcessInterfaceTest = ShardedAggTestFixture;

TEST_F(ShardedProcessInterfaceTest, TestInsert) {
    setupNShards(2);

    const NamespaceString kOutNss = NamespaceString{"unittests-out", "sharded_agg_test"};
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
    expectGetCollection(kOutNss, OID::gen(), ShardKeyPattern{BSON("_id" << 1)});

    // Testing the collection options are propagated.
    const BSONObj collectionOptions = BSON("validationLevel"
                                           << "moderate");
    const BSONObj listCollectionsResponse = BSON("name" << kOutNss.coll() << "type"
                                                        << "collection"
                                                        << "options" << collectionOptions);

    // Mock the response to $out's "listCollections" request.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(kTestAggregateNss, CursorId{0}, {listCollectionsResponse})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Mock the response to $out's "listIndexes" request.
    const BSONObj indexBSON = BSON("_id" << 1);
    const BSONObj listIndexesResponse = BSON("v" << 1 << "key" << indexBSON << "name"
                                                 << "_id_"
                                                 << "ns" << kOutNss.toString());
    onCommand([&](const executor::RemoteCommandRequest& request) {
        return CursorResponse(kTestAggregateNss, CursorId{0}, {listIndexesResponse})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Mock the response to $out's "createCollection" request.
    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        ASSERT(request.cmdObj.hasField("writeConcern")) << request.cmdObj;
        ASSERT_EQ("moderate", request.cmdObj["validationLevel"].str());
        return CursorResponse(kTestAggregateNss, CursorId{0}, {})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Mock the response to $out's "createIndexes" request.
    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        ASSERT(request.cmdObj.hasField("writeConcern")) << request.cmdObj;

        ASSERT(request.cmdObj.hasField("indexes"));
        const std::vector<BSONElement>& indexArray = request.cmdObj["indexes"].Array();
        ASSERT_EQ(1, indexArray.size());
        ASSERT_BSONOBJ_EQ(listIndexesResponse, indexArray.at(0).Obj());

        return CursorResponse(kTestAggregateNss, CursorId{0}, {})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Mock the response to $out's "renameIfOptionsAndIndexesHaveNotChanged" request.
    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        ASSERT(request.cmdObj.hasField("writeConcern")) << request.cmdObj;
        return CursorResponse(kTestAggregateNss, CursorId{0}, {})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo