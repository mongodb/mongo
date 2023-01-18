// Tests resuming change streams when shard key is changed.
//
// @tags: [
//   requires_majority_read_concern,
//   uses_change_streams,
//   requires_fcv_53,
// ]

// Cannot run the filtering metadata check on tests that run refineCollectionShardKey.
TestData.skipCheckShardFilteringMetadata = true;

(function() {
"use strict";

const dbName = 'testDB';
const collName = 'testColl';

const st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: 1,
        enableMajorityReadConcern: '',
        // Use the noop writer with a higher frequency for periodic noops to speed up the test.
        setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}
    }
});

const db = st.s.getDB(dbName);
const coll = db.getCollection(collName);
const changeStream = coll.watch();

const docs = [
    {_id: 0, shardField1: 0, shardField2: 0},
    {_id: 1, shardField1: 1, shardField2: 0},
    {_id: 2, shardField1: 1, shardField2: 1},
    {_id: 3, shardField1: 0, shardField2: 1},
];
const docKeys = [
    {_id: 0},
    {shardField1: 1, _id: 1},
    {shardField1: 1, shardField2: 1, _id: 2},
    {shardField1: 0, shardField2: 1, _id: 3},
];

// Document inserted in unsharded collection.
assert.commandWorked(coll.insert(docs[0]));

// Document inserted in sharded collection.
assert.commandWorked(coll.createIndex({shardField1: 1}));
st.shardColl(collName,
             {shardField1: 1} /* Shard key */,
             {shardField1: 1} /* Split at */,
             {shardField1: 1} /* Move the chunk containing {shardField1: 1} to its own shard */,
             dbName,
             true /* Wait until documents orphaned by the move get deleted */);
assert.commandWorked(coll.insert(docs[1]));

// Document inserted in shard key refined collection.
assert.commandWorked(coll.createIndex({shardField1: 1, shardField2: 1}));
assert.commandWorked(db.adminCommand({
    refineCollectionShardKey: `${dbName}.${collName}`,
    key: {shardField1: 1, shardField2: 1},
}));
assert.commandWorked(coll.insert(docs[2]));

// Insert one more document as a final sentinel, to ensure that there is always at least one visible
// event following the resume points we wish to test.
assert.commandWorked(coll.insert(docs[3]));

const verifyChanges = (changeStream, startingIndex) => {
    const changes = [];
    assert.soon(() => {
        while (changeStream.hasNext()) {
            changes.push(changeStream.next());
        }
        return changes.length === docs.length - startingIndex;
    });
    assert.docEq(docs.slice(startingIndex), changes.map(x => x.fullDocument));
    assert.docEq(docKeys.slice(startingIndex), changes.map(x => x.documentKey));
    return changes;
};

// Verify that we can resume from each change point.
const changes = verifyChanges(changeStream, 0);
changes.forEach((change, i) => {
    verifyChanges(coll.watch([], {resumeAfter: change._id}), i + 1);
});

st.stop();
})();
