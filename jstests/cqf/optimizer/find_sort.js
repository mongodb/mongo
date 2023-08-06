import {
    assertValueOnPath,
    checkCascadesOptimizerEnabled,
    navigateToPlanPath
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const coll = db.cqf_find_sort;
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
const nDocs = 10000;
let numResults = 0;

Random.srand(0);
for (let i = 0; i < nDocs; i++) {
    const va = 100.0 * Random.rand();
    const vb = 100.0 * Random.rand();
    if (va < 5.0 && vb < 5.0) {
        numResults++;
    }
    bulk.insert({a: va, b: vb});
}
assert.gt(numResults, 0);

assert.commandWorked(bulk.execute());

assert.commandWorked(coll.createIndex({a: 1, b: 1}));

const res = coll.explain("executionStats")
                .find({a: {$lt: 5}, b: {$lt: 5}}, {a: 1, b: 1})
                .sort({b: 1})
                .finish();
assert.eq(numResults, res.executionStats.nReturned);

const indexScanNode = navigateToPlanPath(res, "child.child.child.child.leftChild.child");
assertValueOnPath("IndexScan", indexScanNode, "nodeType");
assertValueOnPath(5, indexScanNode, "interval.highBound.bound.0.value");
