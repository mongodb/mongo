/**
 * Tests that the post-image of an update which occurred while the collection was unsharded can
 * still be looked up after the collection becomes sharded. Exercises the fix for SERVER-44484.
 * @tags: [uses_change_streams, requires_fcv_40]
 */
(function() {
    "use strict";

    // Start a new sharded cluster with 2 nodes and obtain references to the test DB and collection.
    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        rs: {nodes: 1, setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}}
    });

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB.test;

    // Open a change stream on the unsharded collection.
    let csCur = mongosColl.watch([], {fullDocument: "updateLookup"});

    // Insert one document with a known _id into the unsharded collection, and obtain its resume
    // token.
    assert.commandWorked(mongosColl.insert({_id: 0, a: -100}));
    assert.soon(() => csCur.hasNext());
    const insertEvent = csCur.next();
    assert.eq(insertEvent.operationType, "insert");

    // Update the document and confirm that we can see the updateLookup fullDocument.
    assert.commandWorked(mongosColl.update({_id: 0}, {$set: {updated: true}}));
    assert.soon(() => csCur.hasNext());
    let updateEvent = csCur.next();
    assert.eq(updateEvent.operationType, "update");
    assert.docEq(updateEvent.fullDocument, {_id: 0, a: -100, updated: true});

    // Now shard the collection on {a: 1} and move the upper chunk to the other shard.
    assert.commandWorked(mongosColl.createIndex({a: 1}));
    st.shardColl(mongosColl, {a: 1}, {a: 0}, {a: 0});

    // Resume a change stream just after the initial insert. We expect the update lookup to succeed,
    // despite the fact that only the _id and not the entire documentKey was recorded in the oplog.
    csCur = mongosColl.watch([], {resumeAfter: insertEvent._id, fullDocument: "updateLookup"});
    assert.soon(() => csCur.hasNext());
    updateEvent = csCur.next();
    assert.eq(updateEvent.operationType, "update");
    assert.docEq(updateEvent.fullDocument, {_id: 0, a: -100, updated: true});

    // Insert a second document with the same _id on the second shard.
    assert.commandWorked(mongosColl.insert({_id: 0, a: 100}));

    // Now that two documents with the same _id are present, the update lookup fails.
    csCur = mongosColl.watch([], {resumeAfter: insertEvent._id, fullDocument: "updateLookup"});
    assert.soon(() => {
        try {
            assert.eq(csCur.hasNext(), false, () => tojson(csCur.next()));
            return false;
        } catch (ex) {
            assert.eq(ex.code, ErrorCodes.ChangeStreamFatalError);
            return true;
        }
    });

    st.stop();
})();
