// Tests that a change stream on a sharded collection, started from a cluster time before an
// insert and a subsequent update, returns the expected 'insert' and 'update' events, and that
// 'fullDocument: "updateLookup"' returns the fully updated document for the 'update' event.
//
// @tags: [
//   assumes_balancer_off,
//   # If a rollback is triggered during a stepdown, the change stream cursor can become invalid.
//   does_not_support_stepdowns,
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
//
load("jstests/libs/collection_drop_recreate.js");

(function() {
"use strict";

function getClusterTime(db) {
    return db.hello().$clusterTime.clusterTime;
}

const st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: 1,
        // Use a higher frequency for periodic noops to speed up the test.
        setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1},
    },
});

const db = st.s0.getDB(jsTestName());

// Ensure the test db primary is st.shard0.shardName.
assert.commandWorked(
    db.adminCommand({enableSharding: db.getName(), primaryShard: st.rs0.getURL()}));

const coll = db[jsTestName()];

{
    // Shard the test collection on 'sk'.
    assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {sk: 1}}));

    // Storing an _id field with dollar-prefixed sub-fields is prohibited.
    // This insert is expected to fail; the rest of the test uses dollar-prefixed sub-fields in the
    // shard key.
    assert.commandFailedWithCode(
        coll.insert({_id: {$v: 2}, sk: 1, val: "original"}),
        ErrorCodes.DollarPrefixedFieldName,
    );
    assertDropCollection(db, coll.getName());
}

const testCases = [
    // Case: document is placed on same shard as the "update" oplog entry.
    {
        name: "local lookup",
        doc: {_id: 1, sk: {$type: 2}, val: "original"},
        prepare: () => {},
    },
    // Case: document is placed on different shard than the "update" oplog entry.
    {
        name: "remote lookup",
        doc: {_id: 1, sk: {$type: 2}, val: "original"},
        prepare: () => {
            // Move the chunk to shard1. The document is now present on shard1, but the update oplog
            // entries are on the primary shard (shard0).
            assert.commandWorked(
                db.adminCommand({
                    moveChunk: coll.getFullName(),
                    // Select any value in the (unsplit) key range to identify the chunk.
                    find: {sk: {test: 0}},
                    to: st.rs1.getURL(),
                    _waitForDelete: true,
                }),
            );
        },
    },
    // Case: using a dollar-prefixed field name that does not exist as an MQL operator.
    {
        name: "unknown operator",
        doc: {_id: 1, sk: {$operatorDoesNotExist: 2}, val: "original"},
        prepare: () => {},
    },
];

// Returns expected full document, using dollar-prefixed shard key field, for various cases.
testCases.forEach((testCase) => {
    const {name, doc, prepare} = testCase;

    assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {sk: 1}}));

    const startTime = getClusterTime(db);

    assert.commandWorked(coll.insert(doc));

    // Make sure document can be retrieved back.
    const lookup = coll.find({_id: doc._id}).toArray();
    assert.eq(1, lookup.length);
    assert.eq(lookup[0], doc);

    // Run an update operation. This will make the updateLookup use the _id and shard key values for
    // looking up the document.
    assert.commandWorked(coll.update({_id: doc._id}, {$set: {val: "changed"}}));

    prepare();

    jsTestLog(`Returns expected full document, using dollar-prefixed shard key field, ${name}`);
    const changeStream = coll.watch([], {
        startAtOperationTime: startTime,
        fullDocument: "updateLookup",
    });

    assert.soon(() => changeStream.hasNext());
    let next = changeStream.next();
    assert.eq(next.operationType, "insert", {next});
    assert.docEq(doc, next.fullDocument);

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "update", {next});
    assert.docEq(Object.assign({}, doc, {val: "changed"}), next.fullDocument);

    changeStream.close();

    assertDropCollection(db, coll.getName());
});

st.stop();
})();
