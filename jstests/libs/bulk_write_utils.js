
/**
 * Helper function to check a BulkWrite cursorEntry.
 */
export const cursorEntryValidator = function(entry, expectedEntry) {
    const assertMsg =
        " value did not match for bulkWrite reply item. actual reply: " + tojson(entry);
    assert.eq(entry.ok, expectedEntry.ok, "'ok'" + assertMsg);
    assert.eq(entry.idx, expectedEntry.idx, "'idx'" + assertMsg);
    assert.eq(entry.n, expectedEntry.n, "'n'" + assertMsg);
    assert.eq(entry.nModified, expectedEntry.nModified, "'nModified' " + assertMsg);
    assert.eq(entry.code, expectedEntry.code, "'code'" + assertMsg);
    assert.docEq(entry.upserted, expectedEntry.upserted, "'upserted' " + assertMsg);
};

export const cursorSizeValidator = function(response, expectedSize) {
    assert.eq(
        response.cursor.firstBatch.length,
        expectedSize,
        "Expected cursor size did not match response cursor size. Response: " + tojson(response));
};

export const summaryFieldsValidator = function(response, fields) {
    const assertMsg =
        " value did not match for bulkWrite summary fields. actual reply: " + tojson(response);
    assert.eq(response.nErrors, fields.nErrors, "nErrors" + assertMsg);
    assert.eq(response.nInserted, fields.nInserted, "nInserted" + assertMsg);
    assert.eq(response.nDeleted, fields.nDeleted, "nDeleted" + assertMsg);
    assert.eq(response.nMatched, fields.nMatched, "nMatched" + assertMsg);
    assert.eq(response.nModified, fields.nModified, "nModified" + assertMsg);
    assert.eq(response.nUpserted, fields.nUpserted, "nUpserted" + assertMsg);
};

// Helper class for the bulkwrite_metrics tests.
export class BulkWriteMetricChecker {
    constructor(testDB, namespace, bulkWrite, fle) {
        this.testDB = testDB;
        this.namespace = namespace;
        this.bulkWrite = bulkWrite;
        this.fle = fle;
    }

    _checkMetricsImpl(status0, top0, {
        updated = 0,
        inserted = 0,
        deleted = 0,
        retriedInsert = 0,
        updateArrayFilters = 0,
        updatePipeline = 0,
        retriedCommandsCount = 0,
        retriedStatementsCount = 0,
        keysExamined = 0
    }) {
        const status1 = this.testDB.serverStatus();
        const top1 = this.testDB.adminCommand({top: 1}).totals[this.namespace];
        const updateField = this.bulkWrite ? "bulkWrite" : "update";

        // Metrics corresponding to CurOp::get(opCtx)->debug().additiveMetrics.
        const doc0 = status0.metrics.document;
        const doc1 = status1.metrics.document;
        assert.eq(doc0.updated + updated, doc1.updated, "document.updated mistmatch");
        assert.eq(doc0.deleted + deleted, doc1.deleted, "document.deleted mistmatch");
        assert.eq(doc0.inserted + inserted, doc1.inserted, "document.inserted mistmatch");
        // Not checking metrics.document.returned or metrics.queryExecutor.scannedObjects as they
        // are not stable across runs of the test, even without bulkWrite.
        const query0 = status0.metrics.queryExecutor;
        const query1 = status1.metrics.queryExecutor;
        assert.eq(query0.scanned + keysExamined, query1.scanned, "queryExecutor.scanned mismatch");

        // Metrics corresponding to Top::get(opCtx->getClient()->getServiceContext()).record(...).
        if (this.fle) {
            // FLE do not set those in Top.
            assert.eq(top0.update, undefined);
            assert.eq(top1.update, undefined);

            assert.eq(top0.remove, undefined);
            assert.eq(top1.remove, undefined);

            assert.eq(top0.insert, undefined);
            assert.eq(top1.insert, undefined);
        } else {
            assert.eq(top0.update.count + updated, top1.update.count, "update.count mismatch");
            assert.eq(top0.remove.count + deleted, top1.remove.count, "remove.count mismatch");
            assert.eq(top0.insert.count + inserted + retriedInsert,
                      top1.insert.count,
                      "insert.count mismatch");
        }

        // Metrics corresponding to ServerWriteConcernMetrics
        const wC0 = status0.opWriteConcernCounters;
        const wC1 = status1.opWriteConcernCounters;

        if (this.fle) {
            // Due to FLE implementation, the actual opWriteConcernCounters metrics logged both for
            // bulkWrite and normal commands are on none, not wmajority.
            // opWriteConcernCounters.updated is also special.
            assert.eq(wC0.update.wmajority, wC1.update.wmajority, "update.wmajority mismatch");
            assert.eq(wC0.delete.wmajority, wC1.delete.wmajority, "delete.wmajority mismatch");
            assert.eq(wC0.insert.wmajority, wC1.insert.wmajority, "insert.wmajority mismatch");
            assert.eq(wC0.update.none + updated / 2, wC1.update.none, "update.none mismatch");
            assert.eq(wC0.delete.none + deleted, wC1.delete.none, "delete.none mismatch");
            assert.eq(wC0.insert.none + inserted, wC1.insert.none, "insert.none mismatch");
        } else {
            assert.eq(
                wC0.update.wmajority + updated, wC1.update.wmajority, "update.wmajority mismatch");
            assert.eq(
                wC0.delete.wmajority + deleted, wC1.delete.wmajority, "delete.wmajority mismatch");
            assert.eq(
                wC0.insert.wmajority + inserted, wC1.insert.wmajority, "insert.wmajority mismatch");

            // All calls are done with {writeConcern: {w: "majority"}} so "none" count should be
            // unchanged.
            assert.eq(wC0.update.none, wC1.update.none, "update.none mismatch");
            assert.eq(wC0.delete.none, wC1.delete.none, "delete.none mismatch");
            assert.eq(wC0.insert.none, wC1.insert.none, "insert.none mismatch");
        }
        // Metrics corresponding to globalOpCounters.gotInsert()/gotDelete()/gotUpdate()
        const op0 = status0.opcounters;
        const op1 = status1.opcounters;
        assert.eq(op0.update + (this.fle ? updated / 2 : updated),
                  op1.update,
                  "opcounters.update mismatch");
        assert.eq(op0.delete + deleted, op1.delete, "opcounters.delete mismatch");
        assert.eq(op0.insert + inserted, op1.insert, "opcounters.insert mismatch");

        // Metrics corresponding to UpdateMetrics.
        if (status1.metrics.commands[updateField] != undefined) {
            let arrayFilters0 = updateArrayFilters;
            let pipeline0 = updatePipeline;
            if (status0.metrics.commands[updateField] != undefined) {
                arrayFilters0 += status0.metrics.commands[updateField].arrayFilters;
                pipeline0 += status0.metrics.commands[updateField].pipeline;
            }

            const update1 = status1.metrics.commands[updateField];
            assert.eq(arrayFilters0, update1.arrayFilters, "update.arrayFilters mismatch");
            assert.eq(pipeline0, update1.pipeline, "update.pipeline mismatch");
        } else {
            assert.eq(
                0, updateArrayFilters, `metrics.commands.${updateField} should not be undefined`);
            assert.eq(0, updatePipeline, `metrics.commands.${updateField} should not be undefined`);
        }

        // Metrics corresponding to RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount
        // and incrementRetriedStatementsCount.
        let t0 = status0.transactions;
        let t1 = status1.transactions;
        assert.eq(t0.retriedCommandsCount + retriedCommandsCount,
                  t1.retriedCommandsCount,
                  "transactions.retriedCommandsCount mismatch");
        assert.eq(t0.retriedStatementsCount + retriedStatementsCount,
                  t1.retriedStatementsCount,
                  "transactions.retriedStatementsCount mismatch");
    }

    checkMetrics(testcaseName, bulkWriteOps, normalCommands, expectedMetrics) {
        print(testcaseName);
        const statusBefore = this.testDB.serverStatus();
        const topBefore = this.testDB.adminCommand({top: 1}).totals[this.namespace];

        if (this.bulkWrite) {
            assert.commandWorked(this.testDB.adminCommand({
                bulkWrite: 1,
                ops: bulkWriteOps,
                nsInfo: [{ns: this.namespace}],
                writeConcern: {w: 'majority'}
            }));
        } else {
            for (let command of normalCommands) {
                command.writeConcern = {w: "majority"};
                assert.commandWorked(this.testDB.runCommand(command));
            }
        }
        this._checkMetricsImpl(statusBefore, topBefore, expectedMetrics);
    }

    checkMetricsWithRetries(
        testcaseName, bulkWriteOps, normalCommand, expectedMetrics, lsid, txnNumber) {
        print(testcaseName);
        let statusBefore = this.testDB.serverStatus();
        let topBefore = this.testDB.adminCommand({top: 1}).totals[this.namespace];

        if (this.bulkWrite) {
            // Second command is a retry, not a duplicated key error.
            for (let i = 0; i < 2; ++i) {
                assert.commandWorked(this.testDB.adminCommand({
                    bulkWrite: 1,
                    ops: bulkWriteOps,
                    nsInfo: [{ns: this.namespace}],
                    lsid: lsid,
                    txnNumber: txnNumber,
                    writeConcern: {w: "majority"}
                }));
            }
        } else {
            normalCommand.writeConcern = {w: "majority"};
            normalCommand.lsid = lsid;
            normalCommand.txnNumber = txnNumber;

            // Second command is a retry.
            for (let i = 0; i < 2; ++i) {
                let res = assert.commandWorked(this.testDB.runCommand(normalCommand));
            }
        }
        this._checkMetricsImpl(statusBefore, topBefore, expectedMetrics);
    }
}
