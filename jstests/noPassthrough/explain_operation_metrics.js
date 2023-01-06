/**
 * Tests explain when profileOperationResourceConsumptionMetrics is set to true and explain
 * verbosity is "executionStats" or "allPlansExecution".
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   requires_wiredtiger
 * ]
 */
(function() {
"use strict";
const dbName = jsTestName();
const collName = 'coll';

const runTest = (db) => {
    for (const verbosity of ["executionStats", "allPlansExecution"]) {
        jsTestLog("Testing with verbosity: " + verbosity);
        const coll = db[collName];
        coll.drop();
        const docs = [{a: 0, b: 0}, {a: 0, b: 0}, {a: 0, b: 0}];
        assert.commandWorked(coll.insertMany(docs));

        const result = assert.commandWorked(coll.find().explain(verbosity));
        assert(result.hasOwnProperty("executionStats"), result);
        const execStats = result.executionStats;
        assert(execStats.hasOwnProperty("operationMetrics"), execStats);
        const operationMetrics = execStats.operationMetrics;
        assert.eq(132, operationMetrics.docBytesRead, result);
        assert.eq(3, operationMetrics.docUnitsRead, result);

        const aggResult =
            assert.commandWorked(coll.explain(verbosity).aggregate({$project: {a: "$a"}}));
        assert(aggResult.hasOwnProperty("executionStats"), aggResult);
        const aggExecStats = aggResult.executionStats;
        assert(aggExecStats.hasOwnProperty("operationMetrics"), aggExecStats);
        const aggOperationMetrics = aggExecStats.operationMetrics;
        assert.eq(132, aggOperationMetrics.docBytesRead, aggResult);
        assert.eq(3, aggOperationMetrics.docUnitsRead, aggResult);

        assert.commandWorked(coll.createIndex({a: 1}));
        const idxFindResult = assert.commandWorked(coll.find({a: 0}).explain(verbosity));
        assert(idxFindResult.hasOwnProperty("executionStats"), idxFindResult);
        const idxFindExecutionStats = idxFindResult.executionStats;
        assert(idxFindExecutionStats.hasOwnProperty("operationMetrics"), idxFindExecutionStats);
        const idxFindOperationMetrics = idxFindExecutionStats.operationMetrics;
        assert.eq(132, idxFindOperationMetrics.docBytesRead, idxFindResult);
        assert.eq(3, idxFindOperationMetrics.docUnitsRead, idxFindResult);
        assert.eq(12, idxFindOperationMetrics.idxEntryBytesRead, idxFindResult);
        assert.eq(3, idxFindOperationMetrics.idxEntryUnitsRead, idxFindResult);

        // The number of cursorSeeks can change depending on whether a yield has occurred. Note
        // however, that the number of calls to 'restoreState' represents an upper bound and not an
        // exact number of cursor seeks. We therefore assert that the number of cursor seeks is at
        // least the number of documents (3) plus the number of (non-yielding) index seeks (1), but
        // is no greater than this quantity plus the number of calls to 'restoreState'.
        const numRestoreStateCalls = idxFindExecutionStats.executionStages.restoreState;
        assert.lte(4, idxFindOperationMetrics.cursorSeeks, idxFindResult);
        assert.gte(4 + numRestoreStateCalls, idxFindOperationMetrics.cursorSeeks, idxFindResult);
    }
};

const setParams = {
    profileOperationResourceConsumptionMetrics: true
};

jsTestLog("Testing standalone");
(function testStandalone() {
    const conn = MongoRunner.runMongod({setParameter: setParams});
    const db = conn.getDB(dbName);
    runTest(db);
    MongoRunner.stopMongod(conn);
})();

jsTestLog("Testing replica set");
(function testReplicaSet() {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: setParams}});
    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB(dbName);
    runTest(db);
    rst.stopSet();
})();
})();
