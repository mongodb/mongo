/**
 * Test that the find command can spill to disk while executing a blocking sort.
 */
import {getAggPlanStage, getPlanStage} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// Only allow blocking sort execution to use 100 kB of memory.
const kMaxMemoryUsageKB = 100;
const kMaxMemoryUsageBytes = kMaxMemoryUsageKB * 1024;

// Construct a document that is just over 1 kB.
const charToRepeat = "-";
const templateDoc = {
    padding: charToRepeat.repeat(1024)
};

const approximateDocumentSize = Object.bsonsize(templateDoc) + 20;

function getFindSortStats(allowDiskUse) {
    let cursor = collection.find().sort({sequenceNumber: -1});
    cursor = cursor.allowDiskUse(allowDiskUse);
    const stageName = isSBEEnabled ? "sort" : "SORT";
    const explain = cursor.explain("executionStats");
    return getPlanStage(explain.executionStats.executionStages, stageName);
}

function getAggregationSortStatsPipelineOptimizedAway() {
    const cursor = collection.explain("executionStats").aggregate([{$sort: {sequenceNumber: -1}}], {
        allowDiskUse: true
    });
    const stageName = isSBEEnabled ? "sort" : "SORT";

    // Use getPlanStage() instead of getAggPlanStage(), because the pipeline is optimized away for
    // this query.
    return getPlanStage(cursor.executionStats.executionStages, stageName);
}

function getAggregationSortStatsForPipeline() {
    const cursor =
        collection.explain("executionStats")
            .aggregate([{$_internalInhibitOptimization: {}}, {$sort: {sequenceNumber: -1}}],
                       {allowDiskUse: true});
    return getAggPlanStage(cursor, "$sort");
}

// Use default maxIteratorsMemoryUsagePercentage, i.e. 10%.
let options = {setParameter: {internalQueryMaxBlockingSortMemoryUsageBytes: kMaxMemoryUsageBytes}};
let conn = MongoRunner.runMongod(options);
assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(options));

let testDb = conn.getDB("test");
let collection = testDb.external_sort_find;
let isSBEEnabled = checkSbeFullyEnabled(testDb);

// Insert data into the collection without exceeding the memory threshold.
let kNumDocsWithinMemLimit = kMaxMemoryUsageKB / 2;
for (let i = 0; i < kNumDocsWithinMemLimit; ++i) {
    templateDoc.sequenceNumber = i;
    assert.commandWorked(collection.insert(templateDoc));
}

// We should be able to successfully sort the collection with or without disk use allowed.
assert.eq(kNumDocsWithinMemLimit,
          collection.find().sort({sequenceNumber: -1}).allowDiskUse(false).itcount());
assert.eq(kNumDocsWithinMemLimit,
          collection.find().sort({sequenceNumber: -1}).allowDiskUse(true).itcount());

// From the total allowed memory 90% is used for data and 10% (up to 1MB) is used to store the file
// iterators.
let kMaxDataMemoryUsageBytes = 0.9 * kMaxMemoryUsageBytes;
// Explain should report that less than kMaxDataMemoryUsageBytes of memory was used, and we did not
// spill to disk. Test that this result is the same whether or not 'allowDiskUse' is set.
let sortStats = getFindSortStats(false);
assert.eq(sortStats.memLimit, kMaxMemoryUsageBytes);
assert.lt(sortStats.totalDataSizeSorted, kMaxDataMemoryUsageBytes);
assert.eq(sortStats.usedDisk, false);
sortStats = getFindSortStats(true);
assert.eq(sortStats.memLimit, kMaxMemoryUsageBytes);
assert.lt(sortStats.totalDataSizeSorted, kMaxDataMemoryUsageBytes);
assert.eq(sortStats.usedDisk, false);

// Add enough data to exceed the memory threshold.
let kNumDocsExceedingMemLimit = kNumDocsWithinMemLimit + kMaxMemoryUsageKB;
for (let i = kNumDocsWithinMemLimit; i < kNumDocsExceedingMemLimit; ++i) {
    templateDoc.sequenceNumber = i;
    assert.commandWorked(collection.insert(templateDoc));
}

// The sort should fail if disk use is not allowed, but succeed if disk use is allowed.
assert.commandFailedWithCode(
    testDb.runCommand(
        {find: collection.getName(), sort: {sequenceNumber: -1}, allowDiskUse: false}),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
assert.eq(kNumDocsExceedingMemLimit,
          collection.find().sort({sequenceNumber: -1}).allowDiskUse().itcount());

// Explain should report that the SORT stage failed if disk use is not allowed.
sortStats = getFindSortStats(false);

// SBE will not report the 'failed' field within sort stats.
if (isSBEEnabled) {
    assert(!sortStats.hasOwnProperty("failed"), sortStats);
} else {
    assert.eq(sortStats.failed, true, sortStats);
}
assert.eq(sortStats.usedDisk, false);
assert.lt(sortStats.totalDataSizeSorted, kMaxDataMemoryUsageBytes);
assert(!sortStats.inputStage.hasOwnProperty("failed"));

// Explain should report that >=kMaxDataMemoryUsageBytes of memory was used, and that we spilled to
// disk.
sortStats = getFindSortStats(true);
assert.eq(sortStats.memLimit, kMaxMemoryUsageBytes);
assert.gte(sortStats.totalDataSizeSorted, kMaxDataMemoryUsageBytes);
assert.eq(sortStats.usedDisk, true);

// If disk use is not allowed but there is a limit, we should be able to avoid exceeding the memory
// limit.
assert.eq(kNumDocsWithinMemLimit,
          collection.find()
              .sort({sequenceNumber: -1})
              .allowDiskUse(false)
              .limit(kNumDocsWithinMemLimit)
              .itcount());

// Create a view on top of the collection. When a find command is run against the view without disk
// use allowed, the command should fail with the expected error code. When the find command allows
// disk use, however, the command should succeed.
assert.commandWorked(testDb.createView("identityView", collection.getName(), []));
let identityView = testDb.identityView;
assert.commandFailedWithCode(
    testDb.runCommand(
        {find: identityView.getName(), sort: {sequenceNumber: -1}, allowDiskUse: false}),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
assert.eq(kNumDocsExceedingMemLimit,
          identityView.find().sort({sequenceNumber: -1}).allowDiskUse().itcount());

// Computing the expected number of spills based on the approximate document size. At this moment
// the number of documents in the collection is 'kNumDocsExceedingMemLimit'
let expectedNumberOfSpills =
    Math.ceil(approximateDocumentSize * kNumDocsExceedingMemLimit / kMaxDataMemoryUsageBytes);

// Verify that performing sorting on the collection using find that exceeds the memory limit results
// in 'expectedNumberOfSpills' when allowDiskUse is set to true.
let findExternalSortStats = getFindSortStats(true);
assert.eq(findExternalSortStats.usedDisk, true, findExternalSortStats);
assert.eq(findExternalSortStats.spills, expectedNumberOfSpills, findExternalSortStats);

// Verify that performing sorting on the collection using aggregate that exceeds the memory limit
// and can be optimized away results in 'expectedNumberOfSpills' when allowDiskUse is set to true.
let aggregationExternalSortStatsForNonPipeline = getAggregationSortStatsPipelineOptimizedAway();
assert.eq(aggregationExternalSortStatsForNonPipeline.usedDisk,
          true,
          aggregationExternalSortStatsForNonPipeline);
assert.eq(aggregationExternalSortStatsForNonPipeline.spills,
          expectedNumberOfSpills,
          aggregationExternalSortStatsForNonPipeline);

// Verify that performing sorting on the collection using aggregate pipeline that exceeds the memory
// limit and can not be optimized away results in 'expectedNumberOfSpills' when allowDiskUse is set
// to true.
let aggregationExternalSortStatsForPipeline = getAggregationSortStatsForPipeline();
assert.eq(aggregationExternalSortStatsForPipeline.usedDisk,
          true,
          aggregationExternalSortStatsForPipeline);
assert.eq(aggregationExternalSortStatsForPipeline.spills,
          expectedNumberOfSpills,
          aggregationExternalSortStatsForPipeline);

MongoRunner.stopMongod(conn);

// Set maxIteratorsMemoryUsagePercentage to minimum, i.e. 0%. In this case only enough memory to
// store 1 file iterator will be used.
let kMaxIteratorsMemoryUsagePercentage = 0.0;
options = {
    setParameter: {
        internalQueryMaxBlockingSortMemoryUsageBytes: kMaxMemoryUsageBytes,
        maxIteratorsMemoryUsagePercentage: kMaxIteratorsMemoryUsagePercentage
    }
};
conn = MongoRunner.runMongod(options);
assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(options));

testDb = conn.getDB("test");
collection = testDb.external_sort_find;
isSBEEnabled = checkSbeFullyEnabled(testDb);

// Insert data into the collection without exceeding the memory threshold.
kNumDocsWithinMemLimit = kMaxMemoryUsageKB / 2;
for (let i = 0; i < kNumDocsWithinMemLimit; ++i) {
    templateDoc.sequenceNumber = i;
    assert.commandWorked(collection.insert(templateDoc));
}

// We should be able to successfully sort the collection with or without disk use allowed.
assert.eq(kNumDocsWithinMemLimit,
          collection.find().sort({sequenceNumber: -1}).allowDiskUse(false).itcount());
assert.eq(kNumDocsWithinMemLimit,
          collection.find().sort({sequenceNumber: -1}).allowDiskUse(true).itcount());

// kMaxIteratorsMemoryUsagePercentage is set to 0%. Only data enough for 1 iterator (~150 bytes)
// will be used.
kMaxDataMemoryUsageBytes = kMaxMemoryUsageBytes - 150;
// Explain should report that less than kMaxDataMemoryUsageBytes of memory was used, and we did not
// spill to disk. Test that this result is the same whether or not 'allowDiskUse' is set.
sortStats = getFindSortStats(false);
assert.eq(sortStats.memLimit, kMaxMemoryUsageBytes);
assert.lt(sortStats.totalDataSizeSorted, kMaxDataMemoryUsageBytes);
assert.eq(sortStats.usedDisk, false);
sortStats = getFindSortStats(true);
assert.eq(sortStats.memLimit, kMaxMemoryUsageBytes);
assert.lt(sortStats.totalDataSizeSorted, kMaxDataMemoryUsageBytes);
assert.eq(sortStats.usedDisk, false);

// Add enough data to exceed the memory threshold.
kNumDocsExceedingMemLimit = kNumDocsWithinMemLimit + kMaxMemoryUsageKB;
for (let i = kNumDocsWithinMemLimit; i < kNumDocsExceedingMemLimit; ++i) {
    templateDoc.sequenceNumber = i;
    assert.commandWorked(collection.insert(templateDoc));
}

// The sort should fail if disk use is not allowed, but succeed if disk use is allowed.
assert.commandFailedWithCode(
    testDb.runCommand(
        {find: collection.getName(), sort: {sequenceNumber: -1}, allowDiskUse: false}),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
assert.eq(kNumDocsExceedingMemLimit,
          collection.find().sort({sequenceNumber: -1}).allowDiskUse().itcount());

// Explain should report that the SORT stage failed if disk use is not allowed.
sortStats = getFindSortStats(false);

// SBE will not report the 'failed' field within sort stats.
if (isSBEEnabled) {
    assert(!sortStats.hasOwnProperty("failed"), sortStats);
} else {
    assert.eq(sortStats.failed, true, sortStats);
}
assert.eq(sortStats.usedDisk, false);
assert.lt(sortStats.totalDataSizeSorted, kMaxDataMemoryUsageBytes);
assert(!sortStats.inputStage.hasOwnProperty("failed"));

// Explain should report that >=kMaxDataMemoryUsageBytes of memory was used, and that we spilled to
// disk.
sortStats = getFindSortStats(true);
assert.eq(sortStats.memLimit, kMaxMemoryUsageBytes);
assert.gte(sortStats.totalDataSizeSorted, kMaxDataMemoryUsageBytes);
assert.eq(sortStats.usedDisk, true);

// If disk use is not allowed but there is a limit, we should be able to avoid exceeding the memory
// limit.
assert.eq(kNumDocsWithinMemLimit,
          collection.find()
              .sort({sequenceNumber: -1})
              .allowDiskUse(false)
              .limit(kNumDocsWithinMemLimit)
              .itcount());

// Create a view on top of the collection. When a find command is run against the view without disk
// use allowed, the command should fail with the expected error code. When the find command allows
// disk use, however, the command should succeed.
assert.commandWorked(testDb.createView("identityView", collection.getName(), []));
identityView = testDb.identityView;
assert.commandFailedWithCode(
    testDb.runCommand(
        {find: identityView.getName(), sort: {sequenceNumber: -1}, allowDiskUse: false}),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
assert.eq(kNumDocsExceedingMemLimit,
          identityView.find().sort({sequenceNumber: -1}).allowDiskUse().itcount());

// maxIteratorsMemoryUsagePercentage is set to 0%. Only one file iterator is allowed. Every time a
// second iterator is created there will be a merge and a spill in order to keep the number of
// iterators to 1.
expectedNumberOfSpills =
    Math.ceil(approximateDocumentSize * kNumDocsExceedingMemLimit / kMaxDataMemoryUsageBytes);
expectedNumberOfSpills = expectedNumberOfSpills + expectedNumberOfSpills - 1;

// Verify that performing sorting on the collection using find that exceeds the memory limit results
// in 'expectedNumberOfSpills' when allowDiskUse is set to true.
findExternalSortStats = getFindSortStats(true);
assert.eq(findExternalSortStats.usedDisk, true, findExternalSortStats);
assert.eq(findExternalSortStats.spills, expectedNumberOfSpills, findExternalSortStats);

// Verify that performing sorting on the collection using aggregate that exceeds the memory limit
// and can be optimized away results in 'expectedNumberOfSpills' when allowDiskUse is set to true.
aggregationExternalSortStatsForNonPipeline = getAggregationSortStatsPipelineOptimizedAway();
assert.eq(aggregationExternalSortStatsForNonPipeline.usedDisk,
          true,
          aggregationExternalSortStatsForNonPipeline);
assert.eq(aggregationExternalSortStatsForNonPipeline.spills,
          expectedNumberOfSpills,
          aggregationExternalSortStatsForNonPipeline);

// Verify that performing sorting on the collection using aggregate pipeline that exceeds the memory
// limit and can not be optimized away results in 'expectedNumberOfSpills' when allowDiskUse is set
// to true.
aggregationExternalSortStatsForPipeline = getAggregationSortStatsForPipeline();
assert.eq(aggregationExternalSortStatsForPipeline.usedDisk,
          true,
          aggregationExternalSortStatsForPipeline);
assert.eq(aggregationExternalSortStatsForPipeline.spills,
          expectedNumberOfSpills,
          aggregationExternalSortStatsForPipeline);

MongoRunner.stopMongod(conn);

// Set maxIteratorsMemoryUsagePercentage to maximum, i.e. 100%. In this case every document will
// cause a spill.
kMaxIteratorsMemoryUsagePercentage = 1.0;
options = {
    setParameter: {
        internalQueryMaxBlockingSortMemoryUsageBytes: kMaxMemoryUsageBytes,
        maxIteratorsMemoryUsagePercentage: kMaxIteratorsMemoryUsagePercentage
    }
};
conn = MongoRunner.runMongod(options);
assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(options));

testDb = conn.getDB("test");
collection = testDb.external_sort_find;
isSBEEnabled = checkSbeFullyEnabled(testDb);

// Even a single document will cause a spill.
for (let i = 0; i < 1; ++i) {
    templateDoc.sequenceNumber = i;
    assert.commandWorked(collection.insert(templateDoc));
}

// The sort should fail if disk use is not allowed, but succeed if disk use is allowed.
assert.commandFailedWithCode(
    testDb.runCommand(
        {find: collection.getName(), sort: {sequenceNumber: -1}, allowDiskUse: false}),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
assert.eq(1, collection.find().sort({sequenceNumber: -1}).allowDiskUse().itcount());

// kMaxIteratorsMemoryUsagePercentage is set to 1.0. All memory is used for the file iterators.
kMaxDataMemoryUsageBytes = 0;
// Explain should report that less than 2 documents memory was used and we did not spill to disk.
sortStats = getFindSortStats(true);
assert.eq(sortStats.memLimit, kMaxMemoryUsageBytes);
assert.gte(sortStats.totalDataSizeSorted, Object.bsonsize(templateDoc));
assert.lt(sortStats.totalDataSizeSorted, 2 * Object.bsonsize(templateDoc));
assert.eq(sortStats.usedDisk, true);

// Add more documents.
kNumDocsExceedingMemLimit = kMaxMemoryUsageKB;
for (let i = 1; i < kNumDocsExceedingMemLimit; ++i) {
    templateDoc.sequenceNumber = i;
    assert.commandWorked(collection.insert(templateDoc));
}

// The sort should fail if disk use is not allowed, but succeed if disk use is allowed.
assert.commandFailedWithCode(
    testDb.runCommand(
        {find: collection.getName(), sort: {sequenceNumber: -1}, allowDiskUse: false}),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
assert.eq(kNumDocsExceedingMemLimit,
          collection.find().sort({sequenceNumber: -1}).allowDiskUse().itcount());

// Explain should report that the SORT stage failed if disk use is not allowed.
sortStats = getFindSortStats(false);

// SBE will not report the 'failed' field within sort stats.
if (isSBEEnabled) {
    assert(!sortStats.hasOwnProperty("failed"), sortStats);
} else {
    assert.eq(sortStats.failed, true, sortStats);
}
assert.eq(sortStats.usedDisk, false);
assert.eq(sortStats.totalDataSizeSorted, 0);
assert(!sortStats.inputStage.hasOwnProperty("failed"));

// Explain should report that kMaxDataMemoryUsageBytes of memory was used, and that we spilled to
// disk.
sortStats = getFindSortStats(true);
assert.eq(sortStats.memLimit, kMaxMemoryUsageBytes);
assert.gte(sortStats.totalDataSizeSorted, kMaxDataMemoryUsageBytes);
assert.eq(sortStats.usedDisk, true);

// Create a view on top of the collection. When a find command is run against the view without disk
// use allowed, the command should fail with the expected error code. When the find command allows
// disk use, however, the command should succeed.
assert.commandWorked(testDb.createView("identityView", collection.getName(), []));
identityView = testDb.identityView;
assert.commandFailedWithCode(
    testDb.runCommand(
        {find: identityView.getName(), sort: {sequenceNumber: -1}, allowDiskUse: false}),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
assert.eq(kNumDocsExceedingMemLimit,
          identityView.find().sort({sequenceNumber: -1}).allowDiskUse().itcount());

// maxIteratorsMemoryUsagePercentage is set to 1.0. Every document will be cause spill and when
// spills are merged at the end there can be only 2 spills used in parallel resulting in 101 more
// spills.
expectedNumberOfSpills = kNumDocsExceedingMemLimit;
let remainingSpills = kNumDocsExceedingMemLimit;
while (remainingSpills > 2) {
    remainingSpills = Math.ceil(remainingSpills / 2);
    expectedNumberOfSpills = expectedNumberOfSpills + remainingSpills;
}

// Verify that performing sorting on the collection using find that exceeds the memory limit results
// in 'expectedNumberOfSpills' when allowDiskUse is set to true.
findExternalSortStats = getFindSortStats(true);
assert.eq(findExternalSortStats.usedDisk, true, findExternalSortStats);
assert.eq(findExternalSortStats.spills, expectedNumberOfSpills, findExternalSortStats);

// Verify that performing sorting on the collection using aggregate that exceeds the memory limit
// and can be optimized away results in 'expectedNumberOfSpills' when allowDiskUse is set to true.
aggregationExternalSortStatsForNonPipeline = getAggregationSortStatsPipelineOptimizedAway();
assert.eq(aggregationExternalSortStatsForNonPipeline.usedDisk,
          true,
          aggregationExternalSortStatsForNonPipeline);
assert.eq(aggregationExternalSortStatsForNonPipeline.spills,
          expectedNumberOfSpills,
          aggregationExternalSortStatsForNonPipeline);

// Verify that performing sorting on the collection using aggregate pipeline that exceeds the memory
// limit and can not be optimized away results in 'expectedNumberOfSpills' when allowDiskUse is set
// to true.
aggregationExternalSortStatsForPipeline = getAggregationSortStatsForPipeline();
assert.eq(aggregationExternalSortStatsForPipeline.usedDisk,
          true,
          aggregationExternalSortStatsForPipeline);
assert.eq(aggregationExternalSortStatsForPipeline.spills,
          expectedNumberOfSpills,
          aggregationExternalSortStatsForPipeline);

MongoRunner.stopMongod(conn);
