/*
 * Tests that descending indexes on range predicates yield all matching documents. Prior to the
 * fix in SERVER-67510, range predicates always scanned from low to high, even when the
 * corresponding index was descending. In this case, the optimizer would start from the "low" point
 * and continue until finding the "high" point, but because the index is descending, the "high"
 * point comes before the "low" point and is out of the index's search scope. To counteract this
 * behavior, the index bounds are swapped when the corresponding index is descending.
 */

import {assertValueOnPlanPath} from "jstests/libs/optimizer_utils.js";

const coll = db.cqf_range_descending;
let docs = [];
/*
 * This is the most basic case: a single range predicate with a descending index.
 */
{
    coll.drop();
    docs.push({a: 1});
    const indexKey = {a: -1};
    assert.commandWorked(coll.createIndex(indexKey));
    for (let i = 0; i < 100; ++i) {
        docs.push({});
    }
    assert.commandWorked(coll.insertMany(docs));
    const query = {a: {$gte: 0, $lte: 2}};
    {
        const res = coll.find(query).hint(indexKey).toArray();
        assert.eq(res.length, 1);
    }
    {
        const res = coll.explain("executionStats").find(query).hint(indexKey).finish();
        assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
    }
}
/*
 * Test a compound index, with a range on the leading field and a descending index on the
 * secondary field.
 */
{
    coll.drop();
    docs = [];
    for (let i = 10; i <= 30; i += 10) {
        for (let j = 1; j <= 3; j++) {
            for (let k = 0; k < 10; k++) {
                docs.push({a: i, b: j});
            }
        }
    }
    // Insert documents that match 'a' but not 'b'--this makes filtering on 'b' more
    // beneficial, and more likely to happen before the fetch. This also discourages
    // collection-scan plans since 'b' is indexed.
    let newDocs = Array.from({length: 1000}, () => ({a: 10, b: 0}));
    // Spread the interesting documents evenly, to encourage IndexScan when sampling in chunks.
    for (let i = 0; i < docs.length; i++) {
        const idx = Math.floor(i * (newDocs.length / docs.length));
        newDocs.push(newDocs[idx]);
        newDocs[idx] = docs[i];
    }
    assert.commandWorked(coll.insertMany(newDocs));
    const indexKey = {a: 1, b: -1};
    assert.commandWorked(coll.createIndex(indexKey));
    const query = {a: {$gte: 10, $lte: 20}, b: {$gt: 1}};
    {
        const res = coll.find(query).hint(indexKey).toArray();
        assert.eq(res.length, 40);
    }
    {
        const res = coll.explain("executionStats").find(query).hint(indexKey).finish();
        assertValueOnPlanPath("IndexScan", res, "child.leftChild.child.nodeType");
    }
}
/*
 * Test a descending index with range predicates, ensuring that the index plan is chosen.
 */
{
    coll.drop();
    docs = [];
    docs.push({a: 1, b: 1});
    for (let i = 0; i < 100; i++) {
        docs.push({a: i + 2, b: i + 2});
    }
    assert.commandWorked(coll.insertMany(docs));
    const indexKey = {a: -1, b: -1};
    assert.commandWorked(coll.createIndex(indexKey));
    const query = [{a: 1}, {_id: 0, a: 1, b: 1}];
    {
        const res = coll.find(...query).hint(indexKey).toArray();
        assert.eq(res.length, 1);
    }
    {
        const res = coll.explain("executionStats").find(...query).hint(indexKey).finish();
        assertValueOnPlanPath("IndexScan", res, "child.child.leftChild.nodeType");
    }
}
