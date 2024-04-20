/**
 * Loading this file overrides Mongo.prototype.runCommand to compare the response from a
 * single-shard cluster connected using replica set endpoint with the response from a single-shard
 * cluster connected using a router.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    runCommandCompareResponsesBase,
} from "jstests/libs/override_methods/rs_endpoint_check_parity_util.js";

assert(TestData.testingReplicaSetEndpoint, "Expect testing replica set endpoint");

const fixtureColl = db.getSiblingDB("config").multiShardedClusterFixture;
const shardedClusterDocs = fixtureColl.find().sort({_id: 1}).toArray();
assert.eq(shardedClusterDocs.length,
          2,
          "Could not find information about the two sharded clusters set up by the fixture " +
              tojsononeline(shardedClusterDocs));
print("Comparing responses from the following sharded clusters: " +
      tojsononeline(shardedClusterDocs));
const globalConn0 = new Mongo(shardedClusterDocs[0].connectionString);
const globalConn1 = new Mongo(shardedClusterDocs[1].connectionString);

// Verify that globalConn0 is a direct connection to a shard in a sharded cluster with replica
// set endpoint enabled.
assert(FeatureFlagUtil.isEnabled(globalConn0, "ReplicaSetEndpoint"));
assert(FixtureHelpers.isReplSet(globalConn0.getDB("admin")));
// Verify that globalConn1 is a connection to a dedicated router of a sharded cluster without
// replica set endpoint enabled.
assert(!FeatureFlagUtil.isEnabled(globalConn1, "ReplicaSetEndpoint"));
assert(FixtureHelpers.isMongos(globalConn1.getDB("admin")));

function runCommandCompareResponses(conn0, dbName, commandName, commandObj, func, makeFuncArgs) {
    assert.eq(conn0.host, globalConn0.host);
    return runCommandCompareResponsesBase(
        conn0, globalConn1, dbName, commandName, commandObj, func, makeFuncArgs);
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/check_parity_sharded_cluster.js");

OverrideHelpers.overrideRunCommand(runCommandCompareResponses);
