/**
 * Tests updates on the size and max document fields of capped collections.
 *
 * @tags: [
 *     requires_fcv_60,
 *     requires_capped,
 *     requires_collstats,
 *     requires_fastcount,
 *     # Capped collections cannot be sharded
 *     assumes_unsharded_collection,
 * ]
 */
(function() {
load("jstests/libs/feature_flag_util.js");

const testDB = db.getSiblingDB(jsTestName());
const cappedColl = testDB["capped_coll"];

const maxSize = 25 * 1024;        // 25 KB.
const doubleMaxSize = 50 * 1024;  // 50 KB.
const maxDocs = 2;
const doubleMaxDocs = 2 * maxDocs;
const initialDocSize = 2;

const maxDocumentCeiling = 0x7fffffff;
const maxSizeCeiling = 0x4000000000000;

let insertDocs = function() {
    // Insert ~50KB of data.
    const doc = {key: "a".repeat(10 * 1024)};
    for (let i = 0; i < 5; i++) {
        assert.commandWorked(cappedColl.insert(doc));
    }
};

let resetCappedCollection = function(extra) {
    const options = Object.assign({}, {capped: true}, extra);
    cappedColl.drop();
    assert.commandWorked(testDB.createCollection(cappedColl.getName(), options));

    // With a capped collection capacity of 25KB, we should have 2 documents.
    insertDocs();
    let stats = assert.commandWorked(cappedColl.stats());
    assert.eq(stats.count, initialDocSize);
    assert.lte(stats.size, extra.size);

    // Check the size and max document limits.
    assert.eq(stats.maxSize, extra.size);
    if (extra.max) {
        assert.eq(stats.max, extra.max);
    }
};

let verifyLimitUpdate = function(updates) {
    const fullCmd = Object.assign({}, {collMod: cappedColl.getName()}, updates);
    assert.commandWorked(testDB.runCommand(fullCmd));
    const stats = assert.commandWorked(cappedColl.stats());

    if (updates.cappedSize) {
        assert.eq(stats.maxSize, updates.cappedSize);
    }
    if (updates.cappedMax) {
        const expectedMax = (updates.cappedMax <= 0) ? maxDocumentCeiling : updates.cappedMax;
        assert.eq(stats.max, expectedMax);
    }
    // Insert documents after updating the capped collection limits. If the actual size is above the
    // limit, the inserts will elict a deletion of documents.
    insertDocs();
};

(function updateSizeLimit() {
    jsTestLog("Updating the maximum size of the capped collection.");
    resetCappedCollection({size: maxSize});

    // Increase the size of the capped collection and we should see more documents in the
    // collection.
    verifyLimitUpdate({cappedSize: doubleMaxSize});
    let stats = assert.commandWorked(cappedColl.stats());
    assert.gt(stats.count, initialDocSize);
    assert.lte(stats.size, doubleMaxSize);

    // Decrease the size parameter of the capped collection and see that documents are removed.
    verifyLimitUpdate({cappedSize: maxSize});
    stats = assert.commandWorked(cappedColl.stats());
    assert.eq(stats.count, initialDocSize);
    assert.lte(stats.size, maxSize);

    // We used to not allow resizing the size of a capped collection below 4096 bytes. This
    // restriction was lifted in SERVER-67036.
    // We should see a reduction in collection size and count relative to the previous test case.
    verifyLimitUpdate({cappedSize: 256});
    stats = assert.commandWorked(cappedColl.stats());
    assert.lt(stats.count, initialDocSize);
    assert.lt(stats.size, maxSize);

    // We expect the resizing of a capped collection to fail when maxSize <= 0 and maxSize >
    // maxSizeCeiling.
    const negativeSize = -1 * maxSize;
    assert.commandFailed(
        testDB.runCommand({collMod: cappedColl.getName(), cappedSize: maxSizeCeiling + 1}));
    assert.commandFailed(testDB.runCommand({collMod: cappedColl.getName(), cappedSize: 0}));
    assert.commandFailed(
        testDB.runCommand({collMod: cappedColl.getName(), cappedSize: negativeSize}));

    // The maximum size can be a non-multiple of 256 bytes.
    // We modify the collection to have a size multiple of 256, then
    // we modify the collection to have a size non multiple of 256 and finally
    // we modify the collection to have a size multiple of 256
    if (FeatureFlagUtil.isEnabled(testDB, "CappedCollectionsRelaxedSize")) {
        verifyLimitUpdate({cappedSize: 25 * 1024});
        verifyLimitUpdate({cappedSize: 50 * 1023});
        verifyLimitUpdate({cappedSize: 50 * 1024});
    }
})();

(function updateMaxLimit() {
    jsTestLog("Updating the maximum document size of the capped collection.");
    resetCappedCollection({size: doubleMaxSize, max: maxDocs});

    // Increase the size of the capped collection and we should see more documents in the
    // collection.
    verifyLimitUpdate({cappedMax: doubleMaxDocs});
    let stats = assert.commandWorked(cappedColl.stats());
    assert.eq(stats.count, doubleMaxDocs);

    // Decrease the size parameter of the capped collection and see that documents are removed.
    verifyLimitUpdate({cappedMax: maxDocs});
    stats = assert.commandWorked(cappedColl.stats());
    assert.eq(stats.count, maxDocs);

    // Setting the maxDocs size to <= 0, we expect the cappedSize to be the only limiting factor.
    const negativeMax = -1 * maxDocs;
    verifyLimitUpdate({cappedMax: negativeMax});
    stats = assert.commandWorked(cappedColl.stats());
    assert.gt(stats.count, initialDocSize);
    assert.lte(stats.size, doubleMaxSize);

    verifyLimitUpdate({cappedMax: 0});
    stats = assert.commandWorked(cappedColl.stats());
    assert.gt(stats.count, initialDocSize);
    assert.lte(stats.size, doubleMaxSize);
})();

(function updateSizeAndMaxLimits() {
    jsTestLog("Updating the maximum size and document limits of the capped collection.");
    resetCappedCollection({size: maxSize, max: maxDocs});

    // Increasing both limits, we should see double the documents.
    verifyLimitUpdate({cappedSize: doubleMaxSize, cappedMax: doubleMaxDocs});
    let stats = assert.commandWorked(cappedColl.stats());
    assert.eq(stats.count, doubleMaxDocs);
    assert.gt(stats.size, maxSize);

    // Decreasing both limits, we should see less documents.
    verifyLimitUpdate({cappedSize: maxSize, cappedMax: maxDocs});
    stats = assert.commandWorked(cappedColl.stats());
    assert.eq(stats.count, maxDocs);
    assert.lte(stats.size, maxSize);

    // Increasing the size limit, but keeping the max low should have no effect.
    verifyLimitUpdate({cappedSize: doubleMaxSize, cappedMax: maxDocs});
    stats = assert.commandWorked(cappedColl.stats());
    assert.eq(stats.count, maxDocs);
    assert.lte(stats.size, doubleMaxSize);

    // Increasing the max limit, but keeping the size limit lower should have no effect.
    verifyLimitUpdate({cappedSize: maxSize, cappedMax: doubleMaxDocs});
    stats = assert.commandWorked(cappedColl.stats());
    assert.eq(stats.count, initialDocSize);
    assert.lte(stats.size, maxSize);
})();
}());
