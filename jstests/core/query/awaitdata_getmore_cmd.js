// Test the awaitData flag for the find/getMore commands.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: getDefaultRWConcern.
//   not_allowed_with_signed_security_token,
//   # This test attempts to perform a getMore command and find it using the currentOp command. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary.
//   assumes_read_preference_unchanged,
//   requires_capped,
//   requires_getmore,
//   uses_multiple_connections,
//   uses_parallel_shell,
// ]

import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

let collName = 'await_data_non_capped';
let coll = db[collName];

// Create a non-capped collection with 10 documents.
jsTestLog('Create a non-capped collection with 10 documents.');
coll.drop();
let docs = [];
for (let i = 0; i < 10; i++) {
    docs.push({a: i});
}
assert.commandWorked(coll.insert(docs));

// Find with tailable flag set should fail for a non-capped collection.
jsTestLog('Find with tailable flag set should fail for a non-capped collection.');
let cmdRes = db.runCommand({find: collName, tailable: true});
assert.commandFailed(cmdRes);

// Should also fail in the non-capped case if both the tailable and awaitData flags are set.
jsTestLog(
    'Should also fail in the non-capped case if both the tailable and awaitData flags are set.');
cmdRes = db.runCommand({find: collName, tailable: true, awaitData: true});
assert.commandFailed(cmdRes);

// With a non-existent collection, should succeed but return no data and a closed cursor.
jsTestLog('With a non-existent collection, should succeed but return no data and a closed cursor.');
collName = 'await_data_missing';
coll = db[collName];
coll.drop();
cmdRes = assert.commandWorked(db.runCommand({find: collName, tailable: true}));
assert.eq(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.firstBatch.length, 0);

// Create a capped collection with 10 documents.
jsTestLog('Create a capped collection with 10 documents.');
collName = 'await_data';  // collection name must match parallel shell task.
coll = db[collName];
coll.drop();
assert.commandWorked(db.createCollection(collName, {capped: true, size: 2048}));
assert.commandWorked(coll.insert(docs));

// GetMore should succeed if query has awaitData but no maxTimeMS is supplied.
jsTestLog('getMore should succeed if query has awaitData but no maxTimeMS is supplied.');
cmdRes = db.runCommand({find: collName, batchSize: 2, awaitData: true, tailable: true});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 2);
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());

// Should also succeed if maxTimeMS is supplied on the original find.
jsTestLog('Should also succeed if maxTimeMS is supplied on the original find.');
const sixtyMinutes = 60 * 60 * 1000;
cmdRes = db.runCommand(
    {find: collName, batchSize: 2, awaitData: true, tailable: true, maxTimeMS: sixtyMinutes});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 2);
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());

// Check that we can set up a tailable cursor over the capped collection.
jsTestLog('Check that we can set up a tailable cursor over the capped collection.');
cmdRes = db.runCommand({find: collName, batchSize: 5, awaitData: true, tailable: true});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 5);

// Check that tailing the capped collection with awaitData eventually ends up returning an empty
// batch after hitting the timeout.
jsTestLog('Check that tailing the capped collection with awaitData eventually ends up returning ' +
          'an empty batch after hitting the timeout.');
cmdRes = db.runCommand({find: collName, batchSize: 2, awaitData: true, tailable: true});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 2);

// Issue getMore until we get an empty batch of results.
jsTestLog('Issue getMore until we get an empty batch of results.');
cmdRes = db.runCommand({
    getMore: cmdRes.cursor.id,
    collection: coll.getName(),
    batchSize: NumberInt(2),
    maxTimeMS: 4000
});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());

// Keep issuing getMore until we get an empty batch after the timeout expires.
jsTestLog('Keep issuing getMore until we get an empty batch after the timeout expires.');
let now;
while (cmdRes.cursor.nextBatch.length > 0) {
    now = new Date();
    cmdRes = db.runCommand({
        getMore: cmdRes.cursor.id,
        collection: coll.getName(),
        batchSize: NumberInt(2),
        maxTimeMS: 4000
    });
    assert.commandWorked(cmdRes);
    jsTestLog('capped collection tailing cursor getMore: ' + now + ': ' + tojson(cmdRes));
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
}
assert.gte((new Date()) - now, 2000);

// Repeat the test, this time tailing the oplog rather than a user-created capped collection.
// The oplog tailing in not possible on mongos.
jsTestLog(
    'Repeat the test, this time tailing the oplog rather than a user-created capped collection.');
if (FixtureHelpers.isReplSet(db)) {
    const localDB = db.getSiblingDB("local");
    const oplogColl = localDB.oplog.rs;

    jsTestLog('Check that tailing the oplog with awaitData eventually ends up returning ' +
              'an empty batch after hitting the timeout.');
    cmdRes = localDB.runCommand({
        find: oplogColl.getName(),
        batchSize: 2,
        awaitData: true,
        tailable: true,
        filter: {ns: {$ne: "config.system.sessions"}}
    });
    assert.commandWorked(cmdRes);
    jsTestLog('Oplog tailing result: ' + tojson(cmdRes));
    if (cmdRes.cursor.id > NumberLong(0)) {
        assert.eq(cmdRes.cursor.ns, oplogColl.getFullName());
        assert.eq(cmdRes.cursor.firstBatch.length, 2);

        jsTestLog('Issue getMore on the oplog until we get an empty batch of results.');
        cmdRes = localDB.runCommand(
            {getMore: cmdRes.cursor.id, collection: oplogColl.getName(), maxTimeMS: 1000});
        assert.commandWorked(cmdRes);
        assert.gt(cmdRes.cursor.id, NumberLong(0));
        assert.eq(cmdRes.cursor.ns, oplogColl.getFullName());

        jsTestLog('Keep issuing getMore on the oplog until we get an empty batch after the ' +
                  'timeout expires.');
        assert.soon(() => {
            now = new Date();
            cmdRes = localDB.runCommand(
                {getMore: cmdRes.cursor.id, collection: oplogColl.getName(), maxTimeMS: 4000});
            assert.commandWorked(cmdRes);
            jsTestLog('oplog tailing cursor getMore: ' + now + ': ' + tojson(cmdRes));
            assert.gt(cmdRes.cursor.id, NumberLong(0));
            assert.eq(cmdRes.cursor.ns, oplogColl.getFullName());
            return cmdRes.cursor.nextBatch.length == 0;
        });
        assert.gte((new Date()) - now, 2000);
    }
}

const originalCmdLogLevel =
    assert.commandWorked(db.setLogLevel(5, 'command')).was.command.verbosity;
const originalQueryLogLevel = assert.commandWorked(db.setLogLevel(5, 'query')).was.query.verbosity;

jsTestLog('Test filtered inserts while writing to a capped collection.');
try {
    // Test filtered inserts while writing to a capped collection.
    // Find with a filter which doesn't match any documents in the collection.
    cmdRes = assert.commandWorked(db.runCommand({
        find: collName,
        batchSize: 2,
        filter: {x: 1},
        awaitData: true,
        tailable: true,
        comment: "uniquifier_comment"
    }));
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.firstBatch.length, 0);

    // The code below will test for the tailable, awaitData cursor being awaken once a matching
    // document is inserted and expects the document to be returned in the next batch. However, the
    // logic without waiting for the timeout in order to receive the data only works if the read
    // concern is not set to majority. The reason for this is that the primary node will notify the
    // waiting cursor on document being inserted on its node and not on the majority of nodes.
    // However, since the read concern is set to majority, the awaken cursor won't find the newly
    // inserted document, as at that time it is present on the primary only. Therefore it will be
    // waiting till the timeout. In order to avoid the waiting we stop running this test if read
    // concern majority.
    const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
    if (topology.type !== Topology.kStandalone) {
        const readConcern =
            assert.commandWorked(db.adminCommand({getDefaultRWConcern: 1})).defaultReadConcern;
        if (readConcern.level == "majority" || TestData.defaultReadConcernLevel === "majority") {
            quit();
        }
    }

    // Test that a getMore command on a tailable, awaitData cursor does not return a new batch to
    // the user if a document was inserted, but it did not match the filter.
    let insertshell = startParallelShell(() => {
        // Signal to the original shell that the parallel shell has successfully started.
        assert.commandWorked(db.await_data.insert({_id: "signal parent shell"}));

        // Wait for the parent shell to start watching for the next document.
        jsTestLog("Checking getMore is being blocked...");
        const filter0 = {
            op: "getmore",
            "cursor.originatingCommand.comment": "uniquifier_comment",
        };
        if (TestData.testingReplicaSetEndpoint) {
            // On the replica set endpoint, currentOp reports both router and shard operations. So
            // filter out one of them.
            filter0.role = "ClusterRole{router}";
        }
        assert.soon(() => db.currentOp(filter0).inprog.length == 1,
                    () => tojson(db.currentOp().inprog));

        // Now write a non-matching document to the collection.
        assert.commandWorked(db.await_data.insert({_id: "no match", x: 0}));

        // Make sure the getMore has not ended after a while.
        sleep(2000);
        jsTestLog("Checking getMore is still being blocked...");
        const filter1 = {op: "getmore", "cursor.originatingCommand.comment": "uniquifier_comment"};
        if (TestData.testingReplicaSetEndpoint) {
            // On the replica set endpoint, currentOp reports both router and shard operations. So
            // filter out one of them.
            filter1.role = "ClusterRole{router}";
        }
        assert.eq(db.currentOp(filter1).inprog.length, 1, tojson(db.currentOp().inprog));

        // Now write a matching document to wake it up.
        jsTestLog("Sending signal to getMore...");
        assert.commandWorked(db.await_data.insert({_id: "match", x: 1}));
        jsTestLog("Insertion shell finished successfully.");
    });

    // Wait until we receive confirmation that the parallel shell has started.
    assert.soon(() => db.await_data.findOne({_id: "signal parent shell"}) !== null);

    // Now issue a getMore which will match the parallel shell's currentOp filter, signalling it to
    // write a non-matching document into the collection. Confirm that we do not receive this
    // document and that we subsequently time out.
    cmdRes = db.runCommand({
        getMore: cmdRes.cursor.id,
        collection: collName,
        maxTimeMS: ReplSetTest.kDefaultTimeoutMS
    });
    assert.commandWorked(cmdRes);
    jsTestLog("Waiting insertion shell to terminate...");
    assert.eq(insertshell(), 0);
    jsTestLog("Insertion shell terminated.");
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
    assert.eq(cmdRes.cursor.nextBatch.length,
              1,
              'Collection documents: ' + tojson(db.await_data.find({}).toArray()));
    assert.docEq({_id: "match", x: 1}, cmdRes.cursor.nextBatch[0]);
} finally {
    db.setLogLevel(originalCmdLogLevel, 'command');
    db.setLogLevel(originalQueryLogLevel, 'query');
}

jsTestLog("Testing tailable cursors with trivially false conditions...");
cmdRes = assert.commandWorked(db.runCommand(
    {find: collName, batchSize: 2, filter: {$alwaysFalse: 1}, awaitData: true, tailable: true}));
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 0);

assert.commandWorked(coll.insert({_id: "new insertion", x: 123}));

cmdRes = assert.commandWorked(
    db.runCommand({getMore: cmdRes.cursor.id, collection: collName, batchSize: 1}));
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.nextBatch.length, 0);
