// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/sharding_environment/cluster_command_test_fixture.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <vector>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


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
    apiVersionErrorsShard1 = {BSON("ns" << "test.ns"
                                        << "code" << 9 << "codeName"
                                        << "APIStrictError"
                                        << "errmsg"
                                        << " Error")};
    apiVersionErrorsShard2 = {BSON("ns" << "test.ns"
                                        << "code" << 19 << "codeName"
                                        << "APIStrictError"
                                        << "errmsg"
                                        << " Error"),
                              BSON("ns" << "test.ns"
                                        << "code" << 19 << "codeName"
                                        << "APIStrictError"
                                        << "errmsg"
                                        << " Error")};
    auto res = runCommandSuccessful(kCommand, false);

    const auto outputFromMongos = OpMsg::parse(res.response).body;
    ASSERT(outputFromMongos.getField("apiVersionErrors").type() == BSONType::array);
    ASSERT_EQ(outputFromMongos.getField("apiVersionErrors").Array().size(), 3);
    ASSERT_FALSE(outputFromMongos.hasField("hasMoreErrors"));
}

TEST_F(ClusterValidateDBMetadataTest, MaxBSONSizeAfterAccumulation) {
    const auto errorObj = BSON("ns" << "test.ns"
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
    ASSERT(outputFromMongos.getField("apiVersionErrors").type() == BSONType::array);
    ASSERT(outputFromMongos.getField("apiVersionErrors").Array().size() <
           2 * apiVersionErrorsShard1.size());
    ASSERT(outputFromMongos.hasField("hasMoreErrors"));
}


}  // namespace
}  // namespace mongo
