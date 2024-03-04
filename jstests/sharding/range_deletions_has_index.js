/**
 * Tests that the _id and uuid indexes are created on config.rangeDeletions on primary step up.
 *
 * @tags: [
 *  requires_fcv_60,
 * ]
 */

const st = new ShardingTest({shards: {rs0: {nodes: 2}}});

// Test index gets created with an empty range deletions collection
let newPrimary = st.rs0.getSecondaries()[0];
st.rs0.awaitReplication();
assert.commandWorked(newPrimary.adminCommand({replSetStepUp: 1}));
st.rs0.waitForPrimary();
const rangeDeletionColl = newPrimary.getDB("config").getCollection("rangeDeletions");
let res = rangeDeletionColl.runCommand({listIndexes: rangeDeletionColl.getName()});
assert.commandWorked(res);
let indexes = res.cursor.firstBatch;

assert.eq(indexes.length, 2);
indexes.forEach((index) => {
    if (bsonWoCompare(index.key, {"_id": 1}) !== 0) {
        assert.eq(bsonWoCompare(index.key, {"collectionUuid": 1, "range.min": 1, "range.max": 1}),
                  0);
    }
});

st.stop();
