/**
 * Verify exchange correctly selects target consumer for documents with MaxKey keystr.
 */

TestData.disableImplicitSessions = true;

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insertMany([{x: 1}, {x: 2}, {x: "a"}, {x: null}]));

// Run the aggregation with a pipeline that emits MaxKey as the 'x' value,
// and an exchange that routes on 'x' with boundaries [MinKey, MaxKey].
const res = db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$project: {x: {$literal: MaxKey()}, _id: 0}}],
    cursor: {batchSize: 10},
    exchange: {
        policy: "keyRange",
        consumers: NumberInt(1),
        key: {x: 1},
        boundaries: [{x: MinKey()}, {x: MaxKey()}],
        consumerIds: [NumberInt(0)],
    },
});

// Validate that the command succeeds and that MaxKey documents are routed to the last
// consumer bucket.
assert.commandWorked(res);

// All four documents should appear (each projected to {x: MaxKey}).
const cursor = new DBCommandCursor(db, res);
const docs = cursor.toArray();
assert.eq(4, docs.length, "Expected 4 documents, got: " + tojson(docs));
for (const doc of docs) {
    assert.eq(MaxKey(), doc.x, "Expected MaxKey field value: " + tojson(doc));
}

coll.drop();
