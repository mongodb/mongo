/**
 * Test that spilling to disk is prohibited in queryable backup (read-only) mode and when the
 * parameter 'recoverFromOplogAsStandalone' is set to 'true'.
 *
 * The 'requires_persistence' tag exludes the test from running with 'inMemory' and
 * 'ephemeralForTest' storage engines that do not support queryable backup (read-only) mode.
 * @tags: [
 *   requires_persistence,
 *   requires_replication
 * ]
 */

(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const memoryLimitMb = 1;
const memoryLimitBytes = 1 * 1024 * 1024;
const largeStr = "A".repeat(1024 * 1024);  // 1MB string

function prepareData(conn) {
    const testDb = conn.getDB(jsTestName());
    testDb.largeColl.drop();
    // Create a collection exceeding the memory limit.
    for (let i = 0; i < memoryLimitMb + 1; ++i)
        assert.commandWorked(testDb.largeColl.insert({x: i, largeStr: largeStr + i}));
    // Create a view on a large collection containg a sort operation.
    testDb.largeView.drop();
    assert.commandWorked(testDb.createView("largeView", "largeColl", [{$sort: {x: -1}}]));
}

function runTest(conn, allowDiskUseByDefault) {
    const testDb = conn.getDB(jsTestName());
    const coll = testDb.largeColl;
    const view = testDb.largeView;

    function assertFailed(cmdObject) {
        assert.commandFailedWithCode(testDb.runCommand(cmdObject),
                                     ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
    }

    assert.commandWorked(
        testDb.adminCommand({setParameter: 1, allowDiskUseByDefault: allowDiskUseByDefault}));

    // The 'aggregate' pipelines running within the memory limit must pass.
    assert.eq(1, coll.aggregate([{$match: {x: 1}}]).itcount());
    assert.eq(1,
              coll.aggregate([{$match: {x: 1}}], {allowDiskUse: !allowDiskUseByDefault}).itcount());

    // The 'aggregate' grouping exceeding the memory limit must fail.
    assertFailed({
        aggregate: coll.getName(),
        pipeline: [{$group: {_id: '$largeStr', minId: {$min: '$_id'}}}],
        cursor: {}
    });
    assertFailed({
        aggregate: coll.getName(),
        pipeline: [{$group: {_id: '$largeStr', minId: {$min: '$_id'}}}],
        cursor: {},
        allowDiskUse: !allowDiskUseByDefault
    });

    // The 'aggregate' sort exceeding the memory limit must fail.
    assertFailed({aggregate: coll.getName(), pipeline: [{$sort: {x: -1}}], cursor: {}});
    assertFailed({
        aggregate: coll.getName(),
        pipeline: [{$sort: {x: -1}}],
        cursor: {},
        allowDiskUse: !allowDiskUseByDefault
    });

    // The 'find' queries within the memory limit must pass.
    assert.eq(1, coll.find({x: 1}).itcount());
    assert.eq(1, coll.find({x: 1}).allowDiskUse(!allowDiskUseByDefault).itcount());

    // The 'find' and sort queries exceeding the memory limit must fail.
    assertFailed({find: coll.getName(), sort: {x: -1}});
    assertFailed({find: coll.getName(), sort: {x: -1}, allowDiskUse: !allowDiskUseByDefault});

    // The 'mapReduce' command running within the memory limit must pass.
    assert.eq(coll.mapReduce(
                      function() {
                          emit("x", 1);
                      },
                      function(k, v) {
                          return Array.sum(v);
                      },
                      {out: {inline: 1}})
                  .results[0]
                  .value,
              memoryLimitMb + 1);

    // The 'mapReduce' command exceeding the memory limit must fail.
    assertFailed({
        mapReduce: coll.getName(),
        map: function() {
            emit("x", this.largeStr);
        },
        reduce: function(k, v) {
            return 42;
        },
        out: {inline: 1}
    });

    // The 'count' command within the memory limit must pass.
    assert.eq(coll.count(), memoryLimitMb + 1);

    // In SBE $sort and $count will not cause spilling, because the largeStr is not saved in memory.
    // Otherwise, the 'count' command exceeding the memory limit must fail.
    if (!checkSBEEnabled(testDb)) {
        assertFailed({count: view.getName()});
    }

    // The 'distinct' command within the memory limit must pass.
    assert.eq(coll.distinct("x"), [0, 1]);

    // The 'distinct' command exceeding the memory limit must fail.
    assertFailed({distinct: view.getName(), key: "largeStr"});
}

// Create a replica set with just one node and add some data.
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
prepareData(primary);
rst.stopSet(/*signal=*/ null, /*forRestart=*/ true);

const tightMemoryLimits = {
    internalDocumentSourceGroupMaxMemoryBytes: memoryLimitBytes,
    internalQueryMaxBlockingSortMemoryUsageBytes: memoryLimitBytes,
    internalQuerySlotBasedExecutionHashAggApproxMemoryUseInBytesBeforeSpill: memoryLimitBytes
};

// Start the mongod in a queryable backup mode with the existing dbpath.
const connQueryableBackup = MongoRunner.runMongod({
    dbpath: primary.dbpath,
    noCleanData: true,
    queryableBackupMode: "",
    setParameter: tightMemoryLimits
});
assert.neq(null, connQueryableBackup, "mongod was unable to start up");

runTest(connQueryableBackup, true);
runTest(connQueryableBackup, false);

MongoRunner.stopMongod(connQueryableBackup);

// Recover the mongod from oplog as a standalone with the existing dbpath.
const connRecoverStandalone = MongoRunner.runMongod({
    dbpath: primary.dbpath,
    noCleanData: true,
    setParameter: Object.assign(tightMemoryLimits, {recoverFromOplogAsStandalone: true})
});
assert.neq(null, connRecoverStandalone, "mongod was unable to start up");

runTest(connRecoverStandalone, true);
runTest(connRecoverStandalone, false);

MongoRunner.stopMongod(connRecoverStandalone);
})();
