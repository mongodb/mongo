(function() {
"use strict";

load("jstests/libs/optimizer_utils.js");  // For checkCascadesOptimizerEnabled.
if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    return;
}

var coll = db.cqf_testCovIndxScan;

coll.drop();

coll.createIndex({f_0: 1, f_1: 1, f_2: 1, f_3: 1, f_4: 1});
coll.getIndexes();

coll.insertMany([
    {f_0: 2, f_1: 8, f_2: 2, f_3: 0, f_4: 2}, {f_0: 7, f_1: 9, f_2: 8, f_3: 3, f_4: 3},
    {f_0: 6, f_1: 6, f_2: 2, f_3: 8, f_4: 3}, {f_0: 9, f_1: 2, f_2: 3, f_3: 5, f_4: 7},
    {f_0: 7, f_1: 8, f_2: 8, f_3: 2, f_4: 9}, {f_0: 7, f_1: 1, f_2: 7, f_3: 3, f_4: 1},
    {f_0: 7, f_1: 3, f_2: 4, f_3: 0, f_4: 7}, {f_0: 8, f_1: 4, f_2: 5, f_3: 6, f_4: 0},
    {f_0: 5, f_1: 2, f_2: 0, f_3: 7, f_4: 0}, {f_0: 0, f_1: 2, f_2: 1, f_3: 9, f_4: 2},
    {f_0: 6, f_1: 0, f_2: 5, f_3: 9, f_4: 1}, {f_0: 0, f_1: 1, f_2: 6, f_3: 8, f_4: 6},
    {f_0: 6, f_1: 5, f_2: 3, f_3: 8, f_4: 5}, {f_0: 2, f_1: 9, f_2: 7, f_3: 2, f_4: 3},
    {f_0: 0, f_1: 6, f_2: 9, f_3: 6, f_4: 8}, {f_0: 5, f_1: 7, f_2: 8, f_3: 1, f_4: 4},
    {f_0: 8, f_1: 5, f_2: 1, f_3: 4, f_4: 6}, {f_0: 6, f_1: 2, f_2: 8, f_3: 4, f_4: 3},
    {f_0: 1, f_1: 6, f_2: 2, f_3: 0, f_4: 3}, {f_0: 1, f_1: 8, f_2: 2, f_3: 5, f_4: 2}
]);

const nDocs = 20;
try {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalCascadesOptimizerFastIndexNullHandling: true}));

    {
        // Covered plan. Also an index scan on all fields is cheaper than a collection scan.
        const res = coll.explain("executionStats").aggregate([
            {'$project': {_id: 0, f_0: 1, f_1: 1, f_2: 1, f_3: 1, f_4: 1}}
        ]);
        assert.eq(nDocs, res.executionStats.nReturned);
        assertValueOnPlanPath("IndexScan", res, "child.child.nodeType");
    }

    {
        // We need to fetch since we do not restrict the set of output fields.
        const res = coll.explain("executionStats").aggregate([
            {'$sort': {f_0: 1, f_1: 1, f_2: 1, f_3: 1, f_4: 1}}
        ]);
        assert.eq(nDocs, res.executionStats.nReturned);
        assertValueOnPlanPath("Seek", res, "child.rightChild.child.nodeType");
        assertValueOnPlanPath("IndexScan", res, "child.leftChild.nodeType");
    }

    {
        // Covered plan.
        const res = coll.explain("executionStats").aggregate([
            {'$project': {_id: 0, f_0: 1, f_1: 1, f_2: 1, f_3: 1, f_4: 1}},
            {'$sort': {f_0: 1, f_1: 1, f_2: 1, f_3: 1, f_4: 1}}
        ]);
        assert.eq(nDocs, res.executionStats.nReturned);
        assertValueOnPlanPath("IndexScan", res, "child.child.nodeType");
    }

    {
        // Covered plan.
        const res = coll.explain("executionStats").aggregate([
            {'$sort': {f_0: 1, f_1: 1, f_2: 1, f_3: 1, f_4: 1}},
            {'$project': {_id: 0, f_0: 1, f_1: 1, f_2: 1, f_3: 1, f_4: 1}}
        ]);
        assert.eq(nDocs, res.executionStats.nReturned);
        assertValueOnPlanPath("IndexScan", res, "child.child.nodeType");
    }
} finally {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalCascadesOptimizerFastIndexNullHandling: false}));
}
}());
