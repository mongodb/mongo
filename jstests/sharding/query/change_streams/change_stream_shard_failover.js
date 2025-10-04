/**
 * Test resuming a change stream on a node other than the one it was started on. Accomplishes this
 * by triggering a stepdown.
 * @tags: [
 *   requires_majority_read_concern,
 *   uses_change_streams,
 * ]
 */

// Checking UUID consistency uses cached connections, which are not valid across restarts or
// stepdowns.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

import {ChangeStreamWatchMode, ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    rs: {nodes: 2, setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}},
    // By default, our test infrastructure sets the election timeout to a very high value (24
    // hours). For this test, we need a shorter election timeout because it relies on nodes running
    // an election when they do not detect an active primary. Therefore, we are setting the
    // electionTimeoutMillis to its default value.
    initiateWithDefaultElectionTimeout: true,
});

const sDB = st.s.getDB("test");
const kCollName = "change_stream_failover";

for (let key of Object.keys(ChangeStreamWatchMode)) {
    const watchMode = ChangeStreamWatchMode[key];
    jsTestLog("Running test for mode " + watchMode);

    const coll = assertDropAndRecreateCollection(sDB, kCollName);

    const nDocs = 100;

    // Split so ids < nDocs / 2 are for one shard, ids >= nDocs / 2 + 1 for another.
    st.shardColl(
        coll,
        {_id: 1}, // key
        {_id: nDocs / 2}, // split
        {_id: nDocs / 2 + 1}, // move
        "test", // dbName
        false, // waitForDelete
    );

    // Be sure we'll only read from the primaries.
    st.s.setReadPref("primary");

    // Open a changeStream.
    const cst = new ChangeStreamTest(ChangeStreamTest.getDBForChangeStream(watchMode, sDB));
    let changeStream = cst.getChangeStream({watchMode: watchMode, coll: coll});

    // Be sure we can read from the change stream. Write some documents that will end up on
    // each shard. Use a bulk write to increase the chance that two of the writes get the same
    // cluster time on each shard.
    const bulk = coll.initializeUnorderedBulkOp();
    const kIds = [];
    for (let i = 0; i < nDocs / 2; i++) {
        // Interleave elements which will end up on shard 0 with elements that will end up on
        // shard 1.
        kIds.push(i);
        bulk.insert({_id: i});
        kIds.push(i + nDocs / 2);
        bulk.insert({_id: i + nDocs / 2});
    }
    // Use {w: "majority"} so that we're still guaranteed to be able to read after the
    // failover.
    assert.commandWorked(bulk.execute({w: "majority"}));

    const firstChange = cst.getOneChange(changeStream);

    // Make one of the primaries step down.
    const oldPrimary = st.rs0.getPrimary();

    assert.commandWorked(oldPrimary.adminCommand({replSetStepDown: 300, force: true}));

    st.rs0.awaitNodesAgreeOnPrimary();
    const newPrimary = st.rs0.getPrimary();
    // Be sure the new primary is not the previous primary.
    assert.neq(newPrimary.port, oldPrimary.port);

    // Read the remaining documents from the original stream.
    const docsFoundInOrder = [firstChange];
    for (let i = 0; i < nDocs - 1; i++) {
        const change = cst.getOneChange(changeStream);
        assert.docEq({db: sDB.getName(), coll: coll.getName()}, change.ns);
        assert.eq(change.operationType, "insert");

        // Needs to be removed here because of multiversion differences between v8.2.0 and previous/following versions.
        delete change.collectionUUID;
        docsFoundInOrder.push(change);
    }

    // Assert that we found the documents we inserted (in any order).
    assert.setEq(new Set(kIds), new Set(docsFoundInOrder.map((doc) => doc.fullDocument._id)));

    // Now resume using the resume token from the first change (which was read before the
    // failover). The mongos should talk to the new primary.
    const resumeCursor = cst.getChangeStream({watchMode: watchMode, coll: coll, resumeAfter: firstChange._id});

    // Be sure we can read the remaining changes in the same order as we read them initially.
    cst.assertNextChangesEqual({cursor: resumeCursor, expectedChanges: docsFoundInOrder.splice(1)});
    cst.cleanUp();

    // Reset the original primary's election timeout.
    assert.commandWorked(oldPrimary.adminCommand({replSetFreeze: 0}));
}

st.stop();
