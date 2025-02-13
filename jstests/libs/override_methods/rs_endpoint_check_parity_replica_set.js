/**
 * Loading this file overrides Mongo.prototype.runCommand to compare the response from a replica set
 * bootstrapped as a single-shard cluster with replica set endpoint enabled with the response from a
 * regular replica set.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {
    isReplicaSet,
    isShardedClusterReplicaSetEndpoint,
    runCommandCompareResponsesBase,
} from "jstests/libs/override_methods/rs_endpoint_check_parity_util.js";

assert(TestData.testingReplicaSetEndpoint, "Expect testing replica set endpoint");

const fixtureColl = db.getSiblingDB("config").multiReplicaSetFixture;
const replDocs = fixtureColl.find().sort({_id: 1}).toArray();
assert.eq(replDocs.length,
          2,
          "Could not find information about the two replica sets set up by the fixture" +
              tojsononeline(replDocs));
jsTest.log.info("Comparing responses from the following replica sets", {replDocs});
const globalConn0 = new Mongo(replDocs[0].connectionString);
const globalConn1 = new Mongo(replDocs[1].connectionString);
assert(isShardedClusterReplicaSetEndpoint(globalConn0));
assert(isReplicaSet(globalConn1));

function runCommandCompareResponses(conn0, dbName, commandName, commandObj, func, makeFuncArgs) {
    assert.eq(conn0.host, globalConn0.host);
    return runCommandCompareResponsesBase(
        conn0, globalConn1, dbName, commandName, commandObj, func, makeFuncArgs);
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/check_parity_replica_set.js");

OverrideHelpers.overrideRunCommand(runCommandCompareResponses);
