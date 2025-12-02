/**
 * Test that $unionWith works when system variables are used after it and pushed in $unionWith
 * during optimization.
 * @tags: [
 *   do_not_wrap_aggregations_in_facets,
 *   assumes_unsharded_collection,
 * ]
 */
const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.coll;
coll.drop();
assert.commandWorked(coll.insertOne({a: 1}));

const date = new Date();
const data =
    coll.aggregate([{$unionWith: {coll: coll.getName(), pipeline: []}}, {$project: {now: "$$NOW"}}])
        .toArray();

assert.eq(data.length, 2);
for (let doc of data) {
    const diff = Math.abs(doc.now.getTime() - date.getTime());
    assert.lte(
        diff, 60000, "Expected $$NOW to be close to current time. Difference was " + diff + "ms");
}

assert.doesNotThrow(() => {
    coll.explain("executionStats").aggregate([
        {$unionWith: {coll: coll.getName(), pipeline: []}},
        {$project: {now: "$$NOW"}},
    ]);
});
