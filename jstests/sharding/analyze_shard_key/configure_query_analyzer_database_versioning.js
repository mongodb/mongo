/**
 * Tests that the configureQueryAnalyzer command uses database versioning when running the
 * listCollections command to validate the collection options.
 *
 * @tags: [requires_fcv_70]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const st = new ShardingTest({mongos: 1, shards: 2, rs: {nodes: 2}});

// Database versioning tests only make sense when all collections are not tracked.
const isTrackUnshardedEnabled = FeatureFlagUtil.isPresentAndEnabled(
    st.s.getDB('admin'), "TrackUnshardedCollectionsUponCreation");
if (isTrackUnshardedEnabled) {
    st.stop();
    quit();
}

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

// Make shard0 the primary shard.
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

const mongos0Coll = st.s.getCollection(ns);
assert.commandWorked(mongos0Coll.createIndex({x: 1}));
assert.commandWorked(mongos0Coll.insert([{x: -1}, {x: 1}]));

const configureCmdObj = {
    configureQueryAnalyzer: ns,
    mode: "full",
    samplesPerSecond: 1
};

// Run the configureQueryAnalyzer command.
assert.commandWorked(st.s.adminCommand(configureCmdObj));

// Make shard1 the primary shard instead by running the movePrimary command.
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard1.name}));

// Rerun the configureQueryAnalyzer command. Since the config server does not know that the primary
// shard has changed, it would send the listCollections command to shard0. Without database
// versioning, the listCollections command would run on shard0 and the configureQueryAnalyzer
// command would fail with a NamespaceNotFound error. With database versioning but without retry
// logic, the listCollections command and the configureQueryAnalyzer command would with a
// StaleDbVersion error.
assert.commandWorked(st.s.adminCommand(configureCmdObj));

st.stop();
