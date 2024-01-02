/*
 * Test to validate the correct behaviour of shardCollection command when finding non retriable
 * errors inside the commit phase.
 *
 * @tags: [
 *    featureFlagAuthoritativeShardCollection,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

// Configure initial sharding cluster
const st = new ShardingTest({shards: {rs0: {nodes: 3}}});

const dbName = jsTestName();
const ns = dbName + ".coll";

let fp = configureFailPoint(st.rs0.getPrimary(), "hangBeforeCommitOnShardingCatalog");

// Start creating a new sharded collection in a parallel shell and hang before committing.
const awaitShardCollection = startParallelShell(
    funWithArgs(function(ns) {
        assert.commandFailedWithCode(db.adminCommand({shardCollection: ns, key: {x: 1}}),
                                     ErrorCodes.InvalidOptions);
    }, ns), st.s.port);

fp.wait();

// Add a zone associated to the shard with an invalid shard key regarding the last shardCollection
// request. This addZone is concurrent with the shardCollection, and it will be effective before
// committing the new collection to the sharding catalog.
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: "A"}));
assert.commandWorked(
    st.s.adminCommand({updateZoneKeyRange: ns, min: {y: 0}, max: {y: 10}, zone: "A"}));

// Force a stepdown to make the coordinator being re-executed and calculate again all non persisted
// variables, i.e. chunk distribution.
st.rs0.freeze(st.rs0.getPrimary());
st.rs0.awaitNodesAgreeOnPrimary();

fp.off();

awaitShardCollection();

// Validate that previous run of the shardCollection command has not left the cluster in an
// inconsistent state and we are able to create the collection successfully.
// TODO SERVER-83774: Check that there is no empty collection on the db primary shard
const inconsistencies = st.s.getDB(dbName).checkMetadataConsistency().toArray();
assert.eq(0, inconsistencies.length, tojson(inconsistencies));

// Use retryWrites when writing to the configsvr because mongos does not automatically retry those.
const mongosSession = st.s.startSession({retryWrites: true});
const configDB = mongosSession.getDatabase("config");
const collEntry = configDB.collections.findOne({_id: ns});
assert.eq(undefined, collEntry);

assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {y: 1}}));

st.stop();
