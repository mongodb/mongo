/**
 * Tests that a change stream can be resumed from the higher of two tokens on separate shards whose
 * clusterTime is identical, differing only by documentKey, without causing the PBRT sent to mongoS
 * to go back-in-time.
 * @tags: [requires_replication, requires_journaling, requires_majority_read_concern]
 */
(function() {
    "use strict";

    const st =
        new ShardingTest({shards: 2, rs: {nodes: 1, setParameter: {writePeriodicNoops: false}}});

    const mongosDB = st.s.getDB(jsTestName());
    const mongosColl = mongosDB.test;

    // Enable sharding on the test DB and ensure its primary is shard0.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Shard on {_id:1}, split at {_id:0}, and move the upper chunk to shard1.
    st.shardColl(mongosColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName(), true);

    // Write one document to each shard.
    assert.commandWorked(mongosColl.insert({_id: -10}));
    assert.commandWorked(mongosColl.insert({_id: 10}));

    // Open a change stream cursor to listen for subsequent events.
    let csCursor = mongosColl.watch([], {cursor: {batchSize: 1}});

    // Update both documents in the collection, such that the events will have the same clusterTime.
    // We update twice to ensure that the PBRT for both shards moves past the first two updates.
    assert.commandWorked(mongosColl.update({}, {$set: {updated: 1}}, {multi: true}));
    assert.commandWorked(mongosColl.update({}, {$set: {updatedAgain: 1}}, {multi: true}));

    // Retrieve the first two events, confirm that they are in order with the same clusterTime.
    let clusterTime = null, updateEvent = null;
    for (let id of[-10, 10]) {
        assert.soon(() => csCursor.hasNext());
        updateEvent = csCursor.next();
        assert.eq(updateEvent.documentKey._id, id);
        clusterTime = (clusterTime || updateEvent.clusterTime);
        assert.eq(updateEvent.clusterTime, clusterTime);
        assert.eq(updateEvent.updateDescription.updatedFields.updated, 1);
    }

    // Update both documents again, so that we will have something to observe after resuming.
    assert.commandWorked(mongosColl.update({}, {$set: {updatedYetAgain: 1}}, {multi: true}));

    // Resume from the second update, and confirm that we only see events starting with the third
    // and fourth updates. We use batchSize:1 to induce mongoD to send each individual event to the
    // mongoS when resuming, rather than scanning all the way to the most recent point in its oplog.
    csCursor = mongosColl.watch([], {resumeAfter: updateEvent._id, cursor: {batchSize: 1}});
    clusterTime = updateEvent = null;
    for (let id of[-10, 10]) {
        assert.soon(() => csCursor.hasNext());
        updateEvent = csCursor.next();
        assert.eq(updateEvent.documentKey._id, id);
        clusterTime = (clusterTime || updateEvent.clusterTime);
        assert.eq(updateEvent.clusterTime, clusterTime);
        assert.eq(updateEvent.updateDescription.updatedFields.updatedAgain, 1);
    }

    st.stop();
})();