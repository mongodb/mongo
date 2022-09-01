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

#include "mongo/db/jsobj.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(BatchedCommandResponseTest, Basic) {
    BSONArray writeErrorsArray(
        BSON_ARRAY(BSON("index" << 0 << "code" << ErrorCodes::IndexNotFound << "errmsg"
                                << "index 0 failed")
                   << BSON("index" << 1 << "code" << ErrorCodes::InvalidNamespace << "errmsg"
                                   << "index 1 failed too")));

    BSONObj writeConcernError(BSON("code" << ErrorCodes::UnknownError << "codeName"
                                          << "UnknownError"
                                          << "errmsg"
                                          << "norepl"
                                          << "errInfo" << BSON("a" << 1)));

    BSONObj origResponseObj =
        BSON("n" << 0 << "opTime" << mongo::Timestamp(1ULL) << "writeErrors" << writeErrorsArray
                 << "writeConcernError" << writeConcernError << "retriedStmtIds"
                 << BSON_ARRAY(1 << 3) << "ok" << 1.0);

    std::string errMsg;
    BatchedCommandResponse response;
    ASSERT_TRUE(response.parseBSON(origResponseObj, &errMsg));

    ASSERT(response.areRetriedStmtIdsSet());
    ASSERT_EQ(response.getRetriedStmtIds().size(), 2);
    ASSERT_EQ(response.getRetriedStmtIds()[0], 1);
    ASSERT_EQ(response.getRetriedStmtIds()[1], 3);

    BSONObj genResponseObj = BSONObjBuilder(response.toBSON()).append("ok", 1.0).obj();
    ASSERT_BSONOBJ_EQ(origResponseObj, genResponseObj);
}

TEST(BatchedCommandResponseTest, StaleConfigInfo) {
    OID epoch = OID::gen();

    StaleConfigInfo staleInfo(
        NamespaceString("TestDB.TestColl"),
        ShardVersion(ChunkVersion({epoch, Timestamp(100, 0)}, {1, 0}),
                     CollectionIndexes({epoch, Timestamp(100, 0)}, boost::none)),
        ShardVersion(ChunkVersion({epoch, Timestamp(100, 0)}, {2, 0}),
                     CollectionIndexes({epoch, Timestamp(100, 0)}, boost::none)),
        ShardId("TestShard"));
    BSONObjBuilder builder(BSON("index" << 0 << "code" << ErrorCodes::StaleConfig << "errmsg"
                                        << "StaleConfig error"));
    staleInfo.serialize(&builder);

    BSONArray writeErrorsArray(BSON_ARRAY(
        builder.obj() << BSON("index" << 1 << "code" << ErrorCodes::InvalidNamespace << "errmsg"
                                      << "index 1 failed too")));

    BSONObj origResponseObj =
        BSON("n" << 0 << "opTime" << mongo::Timestamp(1ULL) << "writeErrors" << writeErrorsArray
                 << "retriedStmtIds" << BSON_ARRAY(1 << 3) << "ok" << 1.0);

    std::string errMsg;
    BatchedCommandResponse response;
    ASSERT_TRUE(response.parseBSON(origResponseObj, &errMsg));
    ASSERT_EQ(0, response.getErrDetailsAt(0).getIndex());
    ASSERT_EQ(ErrorCodes::StaleConfig, response.getErrDetailsAt(0).getStatus().code());
    auto extraInfo = response.getErrDetailsAt(0).getStatus().extraInfo<StaleConfigInfo>();
    ASSERT_EQ(staleInfo.getVersionReceived(), extraInfo->getVersionReceived());
    ASSERT_EQ(*staleInfo.getVersionWanted(), *extraInfo->getVersionWanted());
    ASSERT_EQ(staleInfo.getShardId(), extraInfo->getShardId());
}

TEST(BatchedCommandResponseTest, TooManySmallErrors) {
    BatchedCommandResponse response;

    const auto bigstr = std::string(1024, 'x');

    for (int i = 0; i < 100'000; i++) {
        response.addToErrDetails(write_ops::WriteError(i, {ErrorCodes::BadValue, bigstr}));
    }

    response.setStatus(Status::OK());
    const auto bson = response.toBSON();
    ASSERT_LT(bson.objsize(), BSONObjMaxUserSize);
    const auto errDetails = bson["writeErrors"].Array();
    ASSERT_EQ(errDetails.size(), 100'000u);

    for (int i = 0; i < 100'000; i++) {
        auto errDetail = errDetails[i].Obj();
        ASSERT_EQ(errDetail["index"].Int(), i);
        ASSERT_EQ(errDetail["code"].Int(), ErrorCodes::BadValue);

        if (i < 1024) {
            ASSERT_EQ(errDetail["errmsg"].String(), bigstr) << i;
        } else {
            ASSERT_EQ(errDetail["errmsg"].String(), ""_sd) << i;
        }
    }
}

TEST(BatchedCommandResponseTest, TooManyBigErrors) {
    BatchedCommandResponse response;

    const auto bigstr = std::string(2'000'000, 'x');
    const auto smallstr = std::string(10, 'x');

    for (int i = 0; i < 100'000; i++) {
        response.addToErrDetails(write_ops::WriteError(
            i, {ErrorCodes::BadValue, i < 10 ? bigstr : smallstr /* Don't waste too much RAM */}));
    }

    response.setStatus(Status::OK());
    const auto bson = response.toBSON();
    ASSERT_LT(bson.objsize(), BSONObjMaxUserSize);
    const auto errDetails = bson["writeErrors"].Array();
    ASSERT_EQ(errDetails.size(), 100'000u);

    for (int i = 0; i < 100'000; i++) {
        auto errDetail = errDetails[i].Obj();
        ASSERT_EQ(errDetail["index"].Int(), i);
        ASSERT_EQ(errDetail["code"].Int(), ErrorCodes::BadValue);

        if (i < 2) {
            ASSERT_EQ(errDetail["errmsg"].String(), bigstr) << i;
        } else {
            ASSERT_EQ(errDetail["errmsg"].String(), ""_sd) << i;
        }
    }
}

TEST(BatchedCommandResponseTest, CompatibilityFromWriteErrorToBatchCommandResponse) {
    CollectionGeneration gen(OID::gen(), Timestamp(2, 0));
    ShardVersion versionReceived(ChunkVersion(gen, {1, 0}), CollectionIndexes(gen, boost::none));

    write_ops::UpdateCommandReply reply;
    reply.getWriteCommandReplyBase().setN(1);
    reply.getWriteCommandReplyBase().setWriteErrors(std::vector<write_ops::WriteError>{
        write_ops::WriteError(1,
                              Status(StaleConfigInfo(NamespaceString("TestDB", "TestColl"),
                                                     versionReceived,
                                                     boost::none,
                                                     ShardId("TestShard")),
                                     "Test stale config")),
    });

    BatchedCommandResponse response;
    ASSERT_TRUE(response.parseBSON(reply.toBSON(), nullptr));
    ASSERT_EQ(1U, response.getErrDetails().size());
    ASSERT_EQ(ErrorCodes::StaleConfig, response.getErrDetailsAt(0).getStatus().code());
    ASSERT_EQ("Test stale config", response.getErrDetailsAt(0).getStatus().reason());
    auto staleInfo = response.getErrDetailsAt(0).getStatus().extraInfo<StaleConfigInfo>();
    ASSERT_EQ("TestDB.TestColl", staleInfo->getNss().ns());
    ASSERT_EQ(versionReceived, staleInfo->getVersionReceived());
    ASSERT(!staleInfo->getVersionWanted());
    ASSERT_EQ(ShardId("TestShard"), staleInfo->getShardId());
}

}  // namespace
}  // namespace mongo
