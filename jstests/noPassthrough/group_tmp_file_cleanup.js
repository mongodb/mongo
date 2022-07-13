/**
 * Test that $group cleans up temporary files under dbpath + '/_tmp'.
 */

(function() {
"use strict";

const memoryLimitMb = 16;
const memoryLimitBytes = memoryLimitMb * 1024 * 1024;

// Start mongod with reduced memory limit for the $group stage.
const conn = MongoRunner.runMongod({
    setParameter: {
        internalDocumentSourceGroupMaxMemoryBytes: memoryLimitBytes,
        internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: memoryLimitBytes
    }
});
const testDb = conn.getDB(jsTestName());

// Create a collection exceeding the memory limit.
testDb.largeColl.drop();
const largeStr = "A".repeat(1024 * 1024);  // 1MB string
for (let i = 0; i < memoryLimitMb + 1; ++i)
    assert.commandWorked(testDb.largeColl.insert({x: i, largeStr: largeStr + i}));

// Inhibit optimization so that $group runs in the classic engine.
let pipeline =
    [{$_internalInhibitOptimization: {}}, {$group: {_id: '$largeStr', minId: {$min: '$_id'}}}];

// Make sure that the pipeline needs to spill to disk.
assert.throwsWithCode(() => testDb.largeColl.aggregate(pipeline, {allowDiskUse: false}),
                      ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);

testDb.largeColl.aggregate(pipeline).itcount();
assert.eq(listFiles(conn.dbpath + "/_tmp").length, 0);

// Run the pipeline without $_internalInhibitOptimization so that $group runs in the sbe engine.
pipeline = [{$group: {_id: '$largeStr', minId: {$min: '$_id'}}}];

// Make sure that the pipeline needs to spill to disk.
assert.throwsWithCode(() => testDb.largeColl.aggregate(pipeline, {allowDiskUse: false}),
                      ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
testDb.largeColl.aggregate(pipeline).itcount();
assert.eq(listFiles(conn.dbpath + "/_tmp").length, 0);

MongoRunner.stopMongod(conn);
})();
