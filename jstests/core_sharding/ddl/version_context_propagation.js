/**
 * Tests that sharded DDL operations snapshot the FCV in the versionContext when they start running,
 * set it on their op, and pass it to the participant commands of the distributed DDL.
 *
 * @tags: [
 *   # The test manually places data across shards.
 *   requires_2_or_more_shards,
 *   assumes_balancer_off,
 *   # This test expects FCV to remain constant for the entire duration of the test.
 *   cannot_run_during_upgrade_downgrade,
 *   # This test sets a failpoint so it is not compatible with stepdowns.
 *   does_not_support_stepdowns,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {getPrimaryShardNameForDB} from "jstests/sharding/libs/sharding_util.js";

const collName = jsTestName();
const ns = db.getName() + "." + collName;

// Place a chunk outside the DB primary to ensure the version context must go across the network
assert.commandWorked(db.adminCommand({shardCollection: ns, key: {_id: 1}}));
const primaryShardName = getPrimaryShardNameForDB(db);
const topology = DiscoverTopology.findConnectedNodes(db);
const dataShardName = Object.keys(topology.shards).find(sn => sn != primaryShardName);
assert.commandWorked(db.adminCommand({moveChunk: ns, find: {_id: 0}, to: dataShardName}));

// Hang the drop command on the data shard, so we can take a look at the running ops
const dataShard = new Mongo(topology.shards[dataShardName].primary);
const fpDrop = configureFailPoint(dataShard, "waitAfterCommandFinishesExecution", {
    commands: ['_shardsvrDropCollectionParticipant'],
    ns: ns,
    comment: jsTestName(),
});
const dropThread = new Thread(function(host, dbName, collName) {
    new Mongo(host).getDB(dbName).runCommand({drop: collName, comment: jsTestName()});
}, db.getMongo().host, db.getName(), collName);
dropThread.start();
try {
    fpDrop.wait();

    // Expect version context to be present (or not) according to the feature flag state
    const currentFCV = db.getSiblingDB("admin")
                           .system.version.findOne({_id: 'featureCompatibilityVersion'})
                           .version;
    const expectedVersionContext = FeatureFlagUtil.isEnabled(db, "SnapshotFCVInDDLCoordinators")
        ? {OFCV: currentFCV}
        : undefined;

    // Check that the DDL coordinator has the version context set on its op
    const coordinatorOp = db.getSiblingDB("admin")
                              .aggregate([
                                  {$currentOp: {allUsers: true}},
                                  {
                                      $match: {
                                          shard: primaryShardName,
                                          desc: {$regex: "^ShardingDDLCoordinator"},
                                          'command.comment': jsTestName()
                                      }
                                  }
                              ])
                              .toArray();
    assert.eq(1, coordinatorOp.length, tojson(coordinatorOp));
    assert.docEq(expectedVersionContext, coordinatorOp[0].versionContext, tojson(coordinatorOp[0]));

    // Check that the participant command has received the version context as a command argument
    const participantOp = db.getSiblingDB("admin")
                              .aggregate([
                                  {$currentOp: {allUsers: true}},
                                  {
                                      $match: {
                                          shard: dataShardName,
                                          "command._shardsvrDropCollectionParticipant": collName,
                                          'command.comment': jsTestName()
                                      }
                                  }
                              ])
                              .toArray();
    assert.eq(1, participantOp.length, tojson(participantOp));
    assert.docEq(
        expectedVersionContext, participantOp[0].command.versionContext, tojson(participantOp[0]));
} finally {
    fpDrop.off();
    dropThread.join();
}
