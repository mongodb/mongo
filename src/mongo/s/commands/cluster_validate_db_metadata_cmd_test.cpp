/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/s/commands/cluster_command_test_fixture.h"

namespace mongo {
namespace {

class ClusterValidateDBMetadataTest : public ClusterCommandTestFixture {
protected:
    const BSONObj kCommand{fromjson("{validateDBMetadata: 1, apiParameters: {version: '1'}}")};

    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            cb(request);
            BSONObjBuilder bob;
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }

    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([this, shardIndex](const executor::RemoteCommandRequest& request) {
            BSONObjBuilder bob;
            if (shardIndex == 0) {
                bob.append("apiVersionErrors", apiVersionErrorsShard1);
            } else {
                bob.append("apiVersionErrors", apiVersionErrorsShard2);
            }
            if (hasMoreErrors) {
                bob.append("hasMoreErrors", true);
            }

            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }

    std::vector<BSONObj> apiVersionErrorsShard1;
    std::vector<BSONObj> apiVersionErrorsShard2;
    bool hasMoreErrors = false;
};

TEST_F(ClusterValidateDBMetadataTest, AppendsErrorsFromShards) {
    apiVersionErrorsShard1 = {BSON("ns"
                                   << "test.ns"
                                   << "code" << 9 << "codeName"
                                   << "APIStrictError"
                                   << "errmsg"
                                   << " Error")};
    apiVersionErrorsShard2 = {BSON("ns"
                                   << "test.ns"
                                   << "code" << 19 << "codeName"
                                   << "APIStrictError"
                                   << "errmsg"
                                   << " Error"),
                              BSON("ns"
                                   << "test.ns"
                                   << "code" << 19 << "codeName"
                                   << "APIStrictError"
                                   << "errmsg"
                                   << " Error")};
    auto res = runCommandSuccessful(kCommand, false);

    const auto outputFromMongos = OpMsg::parse(res.response).body;
    ASSERT(outputFromMongos.getField("apiVersionErrors").type() == Array);
    ASSERT_EQ(outputFromMongos.getField("apiVersionErrors").Array().size(), 3);
    ASSERT_FALSE(outputFromMongos.hasField("hasMoreErrors"));
}

TEST_F(ClusterValidateDBMetadataTest, MaxBSONSizeAfterAccumulation) {
    const auto errorObj = BSON("ns"
                               << "test.ns"
                               << "code" << 9 << "codeName"
                               << "APIStrictError"
                               << "errmsg"
                               << " Error");

    // Create two arrays whose size is less than BSONObjMaxUserSize / 2, and verify that the mongos
    // still returns 'hasMoreErrors' flag. This is because we add additional fields like 'shard' to
    // the response from mongos.
    BSONArrayBuilder bob;
    while (bob.len() < BSONObjMaxUserSize / 2) {
        bob.append(errorObj);
        apiVersionErrorsShard1.push_back(errorObj);
    }

    apiVersionErrorsShard2 = apiVersionErrorsShard1;

    auto res = runCommandSuccessful(kCommand, false);

    const auto outputFromMongos = OpMsg::parse(res.response).body;
    ASSERT(outputFromMongos.getField("apiVersionErrors").type() == Array);
    ASSERT(outputFromMongos.getField("apiVersionErrors").Array().size() <
           2 * apiVersionErrorsShard1.size());
    ASSERT(outputFromMongos.hasField("hasMoreErrors"));
}


}  // namespace
}  // namespace mongo
