// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/cluster_command_test_fixture.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"

#include <functional>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

class ClusterCountTest : public ClusterCommandTestFixture {
protected:
    const BSONObj kCountCmdTargeted{fromjson("{count: 'coll', query: {'_id': 0}}")};
    const BSONObj kCountCmdScatterGather{fromjson("{count: 'coll'}")};

    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());
            cb(request);

            BSONObjBuilder bob;
            bob.append("n", 0);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }

    void expectReturnsSuccess(int shardIndex) override {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(kNss.coll(), request.cmdObj.firstElement().valueStringData());

            BSONObjBuilder bob;
            bob.append("n", 0);
            appendTxnResponseMetadata(bob);
            return bob.obj();
        });
    }
};

TEST_F(ClusterCountTest, NoErrors) {
    // count is not allowed in a multi-document transaction.
    testNoErrorsOutsideTransaction(kCountCmdTargeted, kCountCmdScatterGather);
}

TEST_F(ClusterCountTest, IncludeQueryStatsMetrics) {
    testIncludeQueryStatsMetrics(kCountCmdTargeted, true /* isTargeted */);
    testIncludeQueryStatsMetrics(kCountCmdScatterGather, false /* isTargeted */);
}

}  // namespace
}  // namespace mongo
