import {
    checkCascadesOptimizerEnabled,
    navigateToPlanPath,
    runWithFastPathsDisabled,
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const coll = db.cqf_sampling;
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
const nDocs = 10000;

Random.srand(0);
for (let i = 0; i < nDocs; i++) {
    const valA = 10.0 * Random.rand();
    const valB = 10.0 * Random.rand();
    bulk.insert({a: valA, b: valB});
}
assert.commandWorked(bulk.execute());
assert.commandWorked(coll.createIndex({a: 1}));

const res = runWithFastPathsDisabled(() => coll.explain().aggregate([{$match: {'a': {$lt: 2}}}]));
const props = navigateToPlanPath(res, "properties");

// Verify the winning plan cardinality is within roughly 25% of the expected documents.
assert.lt(nDocs * 0.2 * 0.75, props.adjustedCE);
assert.gt(nDocs * 0.2 * 1.25, props.adjustedCE);

// Verify that sampling was used to estimate.
const ceMode = navigateToPlanPath(
    res, "child.properties.logicalProperties.cardinalityEstimate.1.requirementCEs.0.mode");
assert.eq("sampling", ceMode);
