import {
    checkCascadesOptimizerEnabled,
    navigateToPlanPath,
    runWithFastPathsDisabled
} from "jstests/libs/optimizer_utils.js";

if (!checkCascadesOptimizerEnabled(db)) {
    jsTestLog("Skipping test because the optimizer is not enabled");
    quit();
}

const coll = db.cqf_sampling_chunks;
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
const nDocs = 10000;

Random.srand(0);
for (let i = 0; i < nDocs; i++) {
    const valA = 10.0 * Random.rand();
    const valB = 10.0 * Random.rand();
    bulk.insert({a: valA, b: valB});
}
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(bulk.execute());

// For chunk sizes n < 10, 25% accuracy is not guaranteed.
[0, 10, 100, 500, 1000].forEach(n => {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalCascadesOptimizerSampleChunks: n}));
    const res =
        runWithFastPathsDisabled(() => coll.explain().aggregate([{$match: {'a': {$lt: 2}}}]));
    const estimate = navigateToPlanPath(res, "properties.adjustedCE");

    // Verify the winning plan cardinality is within roughly 30% of the expected documents,
    // regardless of the chunk size or whether sampled in chunks or not.
    assert.lt(nDocs * 0.2 * 0.7, estimate);
    assert.gt(nDocs * 0.2 * 1.3, estimate);

    // Verify that sampling was used to estimate.
    const ceMode = navigateToPlanPath(
        res, "child.properties.logicalProperties.cardinalityEstimate.1.requirementCEs.0.mode");
    assert.eq("sampling", ceMode);
});
