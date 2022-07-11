/**
 * Tests that a change stream's update lookup will use the appropriate read concern. In particular,
 * tests that the update lookup will return a version of the document at least as recent as the
 * change that we're doing the lookup for, and that change will be majority-committed.
 * @tags: [
 *   requires_majority_read_concern,
 *   # This test has some timing dependency causing failures when run with a non-streamable rsm
 *   # (e.g. sdam), because non-streamable rsm is generally slower to learn of new replica set info.
 *   requires_streamable_rsm,
 *   uses_change_streams,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/profiler.js");   // For profilerHas*OrThrow() helpers.
load("jstests/replsets/rslib.js");  // For reconfig().

// For stopServerReplication() and restartServerReplication().
load("jstests/libs/write_concern_util.js");

// Configure a replica set to have nodes with specific tags - we will eventually add this as
// part of a sharded cluster.
const rsNodeOptions = {
    setParameter: {
        writePeriodicNoops: true,
        // Note we do not configure the periodic noop writes to be more frequent as we do to
        // speed up other change streams tests, since we provide an array of individually
        // configured nodes, in order to know which nodes have which tags. This requires a step
        // up command to happen, which requires all nodes to agree on an op time. With the
        // periodic noop writer at a high frequency, this can potentially never finish.
    },
    shardsvr: "",
};
const replSetName = jsTestName();

// Note that we include {chainingAllowed: false} in the replica set settings, because this test
// assumes that both secondaries sync from the primary. Without this setting, the
// TopologyCoordinator would sometimes chain one of the secondaries off the other. The test
// later disables replication on one secondary, but with chaining, that would effectively
// disable replication on both secondaries, deadlocking the test.
const rst = new ReplSetTest({
    name: replSetName,
    nodes: [
        {rsConfig: {priority: 1, tags: {tag: "primary"}}},
        {rsConfig: {priority: 0, tags: {tag: "closestSecondary"}}},
        {rsConfig: {priority: 0, tags: {tag: "fartherSecondary"}}}
    ],
    nodeOptions: rsNodeOptions,
    settings: {chainingAllowed: false},
});

rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

// Start the sharding test and add the replica set.
const st = new ShardingTest({manualAddShard: true});
assert.commandWorked(st.s.adminCommand({addShard: replSetName + "/" + rst.getPrimary().host}));

// The default WC is majority and stopServerReplication will prevent satisfying any majority writes.
assert.commandWorked(st.s.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB[jsTestName()];

assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
assert.commandWorked(
    mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

assert.commandWorked(mongosColl.insert({_id: 1}));
rst.awaitReplication();

// Make sure reads with read preference tag 'closestSecondary' go to the tagged secondary.
const closestSecondary = rst.nodes[1];
const closestSecondaryDB = closestSecondary.getDB(mongosDB.getName());
assert.commandWorked(closestSecondaryDB.setProfilingLevel(2));

// Do a read concern "local" read so that the secondary refreshes its metadata.
mongosColl.find().readPref("secondary", [{tag: "closestSecondary"}]);

// We expect the tag to ensure there is only one node to choose from, so the actual read
// preference doesn't really matter - we use 'nearest' throughout.
assert.eq(mongosColl.find()
              .readPref("nearest", [{tag: "closestSecondary"}])
              .comment("testing targeting")
              .itcount(),
          1);
profilerHasSingleMatchingEntryOrThrow({
    profileDB: closestSecondaryDB,
    filter: {ns: mongosColl.getFullName(), "command.comment": "testing targeting"}
});

const changeStreamComment = "change stream against closestSecondary";
const changeStream = mongosColl.aggregate([{$changeStream: {fullDocument: "updateLookup"}}], {
    comment: changeStreamComment,
    $readPreference: {mode: "nearest", tags: [{tag: "closestSecondary"}]}
});
assert.commandWorked(mongosColl.update({_id: 1}, {$set: {updatedCount: 1}}));
assert.soon(() => changeStream.hasNext());
let latestChange = changeStream.next();
assert.eq(latestChange.operationType, "update");
assert.docEq(latestChange.fullDocument, {_id: 1, updatedCount: 1});

// Test that the change stream itself goes to the secondary. There might be more than one if we
// needed multiple getMores to retrieve the changes.
// TODO SERVER-31650 We have to use 'originatingCommand' here and look for the getMore because
// the initial aggregate will not show up.
profilerHasAtLeastOneMatchingEntryOrThrow(
    {profileDB: closestSecondaryDB, filter: {"originatingCommand.comment": changeStreamComment}});

// Test that the update lookup goes to the secondary as well.
let filter = {
    op: "command",
    ns: mongosColl.getFullName(),
    "command.comment": changeStreamComment,
    // We need to filter out any profiler entries with a stale config - this is the first read on
    // this secondary with a readConcern specified, so it is the first read on this secondary that
    // will enforce shard version.
    errCode: {$ne: ErrorCodes.StaleConfig},
    "command.aggregate": mongosColl.getName(),
    "command.pipeline.0.$match._id": 1
};

profilerHasSingleMatchingEntryOrThrow({
    profileDB: closestSecondaryDB,
    filter: filter,
    errorMsgFilter: {ns: mongosColl.getFullName()},
    errorMsgProj: {ns: 1, op: 1, command: 1},
});
// Now add a new secondary which is "closer" (add the "closestSecondary" tag to that secondary,
// and remove it from the old node with that tag) to force update lookups target a different
// node than the change stream itself.
let rsConfig = rst.getReplSetConfig();
rsConfig.members[1].tags = {
    tag: "fartherSecondary"
};
rsConfig.members[2].tags = {
    tag: "closestSecondary"
};
rsConfig.version = rst.getReplSetConfigFromNode().version + 1;
reconfig(rst, rsConfig);
rst.awaitSecondaryNodes();
const newClosestSecondary = rst.nodes[2];
const newClosestSecondaryDB = newClosestSecondary.getDB(mongosDB.getName());
const originalClosestSecondaryDB = closestSecondaryDB;

// Wait for the mongos to acknowledge the new tags from our reconfig.
awaitRSClientHosts(
    st.s, newClosestSecondary, {ok: true, secondary: true, tags: {tag: "closestSecondary"}}, rst);
awaitRSClientHosts(st.s,
                   originalClosestSecondaryDB.getMongo(),
                   {ok: true, secondary: true, tags: {tag: "fartherSecondary"}},
                   rst);
assert.commandWorked(newClosestSecondaryDB.setProfilingLevel(2));

// Make sure new queries with read preference tag "closestSecondary" go to the new secondary.
profilerHasZeroMatchingEntriesOrThrow({profileDB: newClosestSecondaryDB, filter: {}});
assert.eq(mongosColl.find()
              .readPref("nearest", [{tag: "closestSecondary"}])
              .comment("testing targeting")
              .itcount(),
          1);
profilerHasSingleMatchingEntryOrThrow({
    profileDB: newClosestSecondaryDB,
    filter: {ns: mongosColl.getFullName(), "command.comment": "testing targeting"}
});

// Test that the change stream continues on the original host, but the update lookup now targets
// the new, lagged secondary. Even though it's lagged, the lookup should use 'afterClusterTime'
// to ensure it does not return until the node can see the change it's looking up.
stopServerReplication(newClosestSecondary);
assert.commandWorked(mongosColl.update({_id: 1}, {$set: {updatedCount: 2}}));

// Since we stopped replication, we expect the update lookup to block indefinitely until we
// resume replication, so we resume replication in a parallel shell while this thread is blocked
// getting the next change from the stream.
const noConnect = true;  // This shell creates its own connection to the host.
const joinResumeReplicationShell =
        startParallelShell(`load('jstests/libs/write_concern_util.js');

            const pausedSecondary = new Mongo("${newClosestSecondary.host}");

            // Wait for the update lookup to appear in currentOp.
            const changeStreamDB = pausedSecondary.getDB("${mongosDB.getName()}");
            assert.soon(
                function() {
                    return changeStreamDB
                               .currentOpLegacy({
                                   op: "command",
                                   // Note the namespace here happens to be database.$cmd, because
                                   // we're blocked waiting for the read concern, which happens
                                   // before we get to the command processing level and adjust the
                                   // currentOp namespace to include the collection name.
                                   ns: "${mongosDB.getName()}.$cmd",
                                   "command.comment": "${changeStreamComment}",
                               })
                               .inprog.length === 1;
                },
                () => "Failed to find update lookup in currentOp(): " +
                    tojson(changeStreamDB.currentOpLegacy().inprog));

            // Then restart replication - this should eventually unblock the lookup.
            restartServerReplication(pausedSecondary);`,
                           undefined,
                           noConnect);
assert.soon(() => changeStream.hasNext());
latestChange = changeStream.next();
assert.eq(latestChange.operationType, "update");
assert.docEq(latestChange.fullDocument, {_id: 1, updatedCount: 2});
joinResumeReplicationShell();

// Test that the update lookup goes to the new closest secondary.
filter = {
    op: "command",
    ns: mongosColl.getFullName(),
    "command.comment": changeStreamComment,
    // We need to filter out any profiler entries with a stale config - this is the first read on
    // this secondary with a readConcern specified, so it is the first read on this secondary that
    // will enforce shard version.
    errCode: {$ne: ErrorCodes.StaleConfig},
    "command.aggregate": mongosColl.getName()
};

profilerHasSingleMatchingEntryOrThrow({profileDB: newClosestSecondaryDB, filter: filter});

changeStream.close();
st.stop();
rst.stopSet();
}());
