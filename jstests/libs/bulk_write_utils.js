
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

// assert.eq(before + diff, after, errorMessage) but with more information on failure.
const checkEqual = function(before, expectedDiff, after, errorMessage) {
    assert.eq(
        before + expectedDiff, after, `${errorMessage}: ${before} + ${expectedDiff} != ${after}`);
};

// Helper class for the bulkwrite_metrics tests.
export class BulkWriteMetricChecker {
    constructor(testDB,
                namespaces,
                bulkWrite,
                isMongos,
                fle,
                errorsOnly,
                retryCount = 3,
                timeseries = false,
                defaultTimestamp = undefined) {
        this.testDB = testDB;
        this.namespaces = namespaces;
        this.bulkWrite = bulkWrite;
        this.isMongos = isMongos;
        this.fle = fle;
        this.retryCount = retryCount;
        this.errorsOnly = errorsOnly;
        this.timeseries = timeseries;
        this.defaultTimestamp = defaultTimestamp;
    }

    // Metrics corresponding to
    // Top::get(opCtx->getClient()->getServiceContext()).record(...).
    _checkTopMetrics(top0,
                     top1,
                     inserted,
                     retriedInsert,
                     updateCount,
                     deleteCount,
                     perNamespaceMetrics,
                     nsIndicesInRequest) {
        if (this.fle) {
            // FLE do not set those in Top due to redaction.
            for (const ns of this.namespaces) {
                assert.eq(top0[ns].update, undefined);
                assert.eq(top1[ns].update, undefined);

                assert.eq(top0[ns].remove, undefined);
                assert.eq(top1[ns].remove, undefined);

                assert.eq(top0[ns].insert, undefined);
                assert.eq(top1[ns].insert, undefined);
            }
        } else {
            for (const idx of nsIndicesInRequest) {
                const ns = this.namespaces[idx];
                if (perNamespaceMetrics != undefined) {
                    inserted = perNamespaceMetrics[ns].inserted ?? 0;
                    updateCount = perNamespaceMetrics[ns].updateCount ?? 0;
                    deleteCount = perNamespaceMetrics[ns].deleteCount ?? 0;
                    retriedInsert = perNamespaceMetrics[ns].retriedInsert ?? 0;
                }
                checkEqual(top0[ns].update.count,
                           updateCount,
                           top1[ns].update.count,
                           `update.count mismatch for ${ns}`);
                checkEqual(top0[ns].remove.count,
                           deleteCount,
                           top1[ns].remove.count,
                           "remove.count mismatch");
                checkEqual(top0[ns].insert.count,
                           inserted + retriedInsert,
                           top1[ns].insert.count,
                           `insert.count mismatch for ${ns}`);
            }
        }
    }

    // Metrics corresponding to CurOp::get(opCtx)->debug().additiveMetrics.
    _checkAdditiveMetrics(
        status0, status1, actualInserts, updated, fleSafeContentUpdates, deleted) {
        const doc0 = status0.metrics.document;
        const doc1 = status1.metrics.document;

        checkEqual(doc0.updated,
                   updated + fleSafeContentUpdates,
                   doc1.updated,
                   "document.updated mismatch");
        checkEqual(doc0.deleted, deleted, doc1.deleted, "document.deleted mismatch");
        checkEqual(doc0.inserted, actualInserts, doc1.inserted, "document.inserted mismatch");
    }

    // Metrics corresponding to ServerWriteConcernMetrics
    _checkWriteConcernMetrics(status0,
                              status1,
                              inserted,
                              actualInserts,
                              updateCount,
                              fleSafeContentUpdates,
                              deleteCount,
                              deleted,
                              retryCount) {
        const wC0 = status0.opWriteConcernCounters;
        const wC1 = status1.opWriteConcernCounters;

        if (this.fle) {
            // Due to FLE implementation, the actual opWriteConcernCounters metrics logged both
            // for bulkWrite and normal commands are using the implicit default write concern, not
            // wmajority. FLE update is a findAndModify followed by an a safeContent update.
            checkEqual(wC0.update.wmajority, 0, wC1.update.wmajority, "update.wmajority mismatch");
            checkEqual(wC0.delete.wmajority, 0, wC1.delete.wmajority, "delete.wmajority mismatch");
            checkEqual(wC0.insert.wmajority, 0, wC1.insert.wmajority, "insert.wmajority mismatch");
            checkEqual(
                wC0.update.none, fleSafeContentUpdates, wC1.update.none, "update.none mismatch");
            checkEqual(wC0.delete.none, deleted, wC1.delete.none, "delete.none mismatch");
            checkEqual(wC0.insert.none, actualInserts, wC1.insert.none, "insert.none mismatch");
        } else {
            // All calls are done with {writeConcern: {w: "majority"}} so "none" count should be
            // unchanged, except for timeseries.
            // TODO SERVER-84799 timeseries condition below and comment above.
            const [uNone, uMaj] =
                (this.timeseries && retryCount > 1) ? [updateCount, 0] : [0, updateCount];
            const [dNone, dMaj] =
                (this.timeseries && retryCount > 1) ? [deleteCount, 0] : [0, deleteCount];
            checkEqual(
                wC0.update.wmajority, uMaj, wC1.update.wmajority, "update.wmajority mismatch");
            checkEqual(
                wC0.delete.wmajority, dMaj, wC1.delete.wmajority, "delete.wmajority mismatch");
            checkEqual(
                wC0.insert.wmajority, inserted, wC1.insert.wmajority, "insert.wmajority mismatch");

            checkEqual(wC0.update.none, uNone, wC1.update.none, "update.none mismatch");
            checkEqual(wC0.delete.none, dNone, wC1.delete.none, "delete.none mismatch");
            checkEqual(wC0.insert.none, 0, wC1.insert.none, "insert.none mismatch");
        }
    }

    // Metrics corresponding to UpdateMetrics.
    _checkUpdateMetrics(status0, status1, updateArrayFilters, updatePipeline) {
        // For bulkWrite, status1.metrics.commands.bulkWrite can exist even without
        // arrayFilters and pipeline being set since it also counts "total" and "failed".
        const updateField = this.bulkWrite ? "bulkWrite" : "update";

        const update1 = status1.metrics.commands[updateField];
        const update0 = status0.metrics.commands[updateField];
        if (update1 != undefined &&
            (update1.arrayFilters != undefined || update1.pipeline != undefined)) {
            let arrayFilters0 = 0;
            let pipeline0 = 0;
            if (update0 != undefined) {
                arrayFilters0 = update0.arrayFilters;
                pipeline0 = update0.pipeline;
            }

            let arrayFiltersDiff = updateArrayFilters;

            checkEqual(arrayFilters0,
                       arrayFiltersDiff,
                       update1.arrayFilters,
                       "update.arrayFilters mismatch");
            checkEqual(pipeline0, updatePipeline, update1.pipeline, "update.pipeline mismatch");
        } else {
            assert.eq(
                0, updateArrayFilters, `metrics.commands.${updateField} should not be undefined`);
            assert.eq(0, updatePipeline, `metrics.commands.${updateField} should not be undefined`);
        }
    }

    // Metrics corresponding to globalOpCounters.gotInsert() / gotDelete() / gotUpdate()
    _checkOpCounters(status0,
                     status1,
                     inserted,
                     actualInserts,
                     updateCount,
                     updateManyCount,
                     opcounterFactor,
                     fleSafeContentUpdates,
                     deleteCount,
                     deleteManyCount,
                     retryCount) {
        let op0 = status0.opcounters;
        let op1 = status1.opcounters;
        // For BulkWrite, each statement is always a single insert/update so `inserted` and
        // `updated` also are the number of statements. Also BulkWrite FLE does not support mixing
        // updates with inserts.
        let numberOfInsertStatements = inserted;
        let numberOfUpdateStatements = updateCount;

        let opUpdated = updateCount;
        let opDeleted = deleteCount;
        let opInserted = actualInserts;

        let opUpdateMany = updateManyCount;
        let opDeleteMany = deleteManyCount;

        if (this.isMongos) {
            // TODO SERVER-84798 timeseries should increase by retryCount too.
            if (!this.timeseries) {
                opInserted *= retryCount;
            }
            opDeleted *= retryCount;
            opUpdated *= retryCount;
            opUpdateMany *= retryCount;
            opDeleteMany *= retryCount;
            if (this.fle) {
                // On Mongos, there is one extra opcounter increment per statement.
                opDeleted *= 2;
                opDeleteMany *= 2;
                if (numberOfInsertStatements >= 0) {
                    opInserted = (numberOfInsertStatements + actualInserts) * retryCount;
                }
                // FLE2 updates don't execute the safeContent updates on retries, only the
                // findAndModify step.
                opUpdated = (this.bulkWrite ? 0 : numberOfUpdateStatements) * retryCount +
                    fleSafeContentUpdates;
                opUpdateMany = (this.bulkWrite ? 0 : updateManyCount) * retryCount;
            }
        } else if (this.fle) {
            opUpdated = fleSafeContentUpdates;
        }

        checkEqual(
            op0.update, opUpdated * opcounterFactor, op1.update, "opcounters.update mismatch");
        checkEqual(op0.delete, opDeleted, op1.delete, "opcounters.delete mismatch");
        checkEqual(op0.insert, opInserted, op1.insert, "opcounters.insert mismatch");

        // TODO: SERVER-103358 currently we increment for internal retries. The code block below
        // allows an extra updateManyCount in case an internal retry incremented the counter an
        // extra time. Remove this special case once this is fixed.
        if (this.bulkWrite && this.isMongos && updateManyCount > 0) {
            const before = status0.metrics.query.updateManyCount;
            const afterActual = status1.metrics.query.updateManyCount;
            // We add one for an extra retry and one because the range is exclusive.
            const afterUpperBound = status0.metrics.query.updateManyCount + opUpdateMany + 2;

            const errMsg = `metrics.query.updateMany mismatch: ${afterActual}} + is not between ${
                before} and ${afterUpperBound}`;
            assert.between(before, afterActual, afterUpperBound, errMsg, false /* inclusive */);
        } else {
            checkEqual(status0.metrics.query.updateManyCount,
                       opUpdateMany,
                       status1.metrics.query.updateManyCount,
                       "metrics.query.updateMany mismatch");
        }

        checkEqual(status0.metrics.query.deleteManyCount,
                   opDeleteMany,
                   status1.metrics.query.deleteManyCount,
                   "metrics.query.deleteMany mismatch");
    }

    _checkMongodOnlyMetrics(status0,
                            top0,
                            status1,
                            top1,
                            updated,
                            updateCount,
                            inserted,
                            deleted,
                            deleteCount,
                            retriedInsert,
                            retriedCommandsCount,
                            retriedStatementsCount,
                            fleSafeContentUpdates,
                            actualInserts,
                            retryCount,
                            perNamespaceMetrics,
                            nsIndicesInRequest) {
        this._checkAdditiveMetrics(
            status0, status1, actualInserts, updated, fleSafeContentUpdates, deleted);

        // Not checking metrics.document.returned or metrics.queryExecutor.scannedObjects as
        // they are not stable across runs of the test, even without bulkWrite.
        // metrics.queryExecutor.scanned is stable but the FLE logic for it is very complicated
        // to maintain here.

        this._checkTopMetrics(top0,
                              top1,
                              inserted,
                              retriedInsert,
                              updateCount,
                              deleteCount,
                              perNamespaceMetrics,
                              nsIndicesInRequest);

        this._checkWriteConcernMetrics(status0,
                                       status1,
                                       inserted,
                                       actualInserts,
                                       updateCount,
                                       fleSafeContentUpdates,
                                       deleteCount,
                                       deleted,
                                       retryCount);

        // Metrics corresponding to
        // RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount
        // and incrementRetriedStatementsCount.
        let t0 = status0.transactions;
        let t1 = status1.transactions;
        checkEqual(t0.retriedCommandsCount,
                   retriedCommandsCount,
                   t1.retriedCommandsCount,
                   "transactions.retriedCommandsCount mismatch");
        checkEqual(t0.retriedStatementsCount,
                   retriedStatementsCount,
                   t1.retriedStatementsCount,
                   "transactions.retriedStatementsCount mismatch");
    }

    _checkMongosOnlyMetrics(status0,
                            status1,
                            updateCount,
                            inserted,
                            deleteCount,
                            eqIndexedEncryptedFields,
                            singleUpdateForBulkWrite,
                            singleInsertForBulkWrite,
                            singleDeleteForBulkWrite,
                            insertShardField,
                            updateShardField,
                            deleteShardField,
                            fleSafeContentUpdates,
                            retryCount,
                            actualInserts) {
        const targeted0 = status0.shardingStatistics.numHostsTargeted;
        const targeted1 = status1.shardingStatistics.numHostsTargeted;

        let targetedUpdate = updateCount * retryCount;
        let targetedDelete = deleteCount * retryCount;
        let targetedInsert = actualInserts;
        let unshardedInsert = 0;

        if (this.fle) {
            targetedUpdate = fleSafeContentUpdates;
            // BulkWrite FLE does not allow mixing insert and update so updated != 0 means
            // it is an FLE update.
            if (updateCount != 0) {
                targetedInsert = 0;
            } else {
                targetedInsert = inserted;
            }
            // The FLE inserts in the state collection are batched in a single command so they count
            // as 1 here, unlike for opcounters. eqIndexedEncryptedFields is per insert/update and
            // we don't allow mixing insert and update for FLE bulkWrite.
            unshardedInsert = 2 * (inserted + updateCount) * (eqIndexedEncryptedFields > 0 ? 1 : 0);
            unshardedInsert *= retryCount;
        }

        // TODO SERVER-84798 timeseries should increase by retryCount too.
        if (!this.timeseries) {
            targetedInsert *= retryCount;
        }

        if (this.timeseries && !this.bulkWrite && updateShardField === "manyShards") {
            targetedUpdate = targetedUpdate * 2;
        }

        if (this.timeseries && !this.bulkWrite && deleteShardField === "manyShards") {
            targetedDelete = targetedDelete * 2;
        }

        if (this.bulkWrite) {
            if (singleUpdateForBulkWrite) {
                targetedUpdate = 1;
            }

            if (singleInsertForBulkWrite) {
                targetedInsert = 1;
            }

            if (singleDeleteForBulkWrite) {
                targetedDelete = 1;
            }
        }

        checkEqual(targeted0.insert[insertShardField],
                   targetedInsert,
                   targeted1.insert[insertShardField],
                   `insert.${insertShardField} mismatch`);

        checkEqual(targeted0.insert.unsharded,
                   unshardedInsert,
                   targeted1.insert.unsharded,
                   "insert.unsharded mismatch");

        checkEqual(targeted0.update[updateShardField],
                   targetedUpdate,
                   targeted1.update[updateShardField],
                   `update.${updateShardField} mismatch`);

        checkEqual(targeted0.delete[deleteShardField],
                   targetedDelete,
                   targeted1.delete[deleteShardField],
                   `delete.${deleteShardField} mismatch`);
    }

    // eqIndexedEncryptedFields is per insert/update in the command.
    _checkMetricsImpl(status0, top0, nsIndicesInRequest, {
        updated = 0,
        updateCount = undefined,
        updateManyCount = 0,
        inserted = 0,
        deleted = 0,
        deleteCount = undefined,
        deleteManyCount = 0,
        eqIndexedEncryptedFields = 0,
        retriedInsert = 0,
        updateArrayFilters = 0,
        updatePipeline = 0,
        singleUpdateForBulkWrite = false,
        singleInsertForBulkWrite = false,
        singleDeleteForBulkWrite = false,
        insertShardField = "oneShard",
        updateShardField = this.timeseries ? "oneShard" : "allShards",
        deleteShardField = this.timeseries ? "oneShard" : "allShards",
        retryCount = 0,
        opcounterFactor = 1,
        perNamespaceMetrics = undefined
    }) {
        // updateCount is the number of update commands, it is different from updated when
        // multi: true.
        if (updateCount == undefined) {
            updateCount = updated;
        }
        if (deleteCount == undefined) {
            deleteCount = deleted;
        }
        const status1 = this.testDB.serverStatus();

        // An FLE update causes one findAndModify followed by an optional (absent if
        // eqIndexedEncryptedFields == 0) update.
        let fleSafeContentUpdates = (updated > 0 && eqIndexedEncryptedFields > 0) ? 1 : 0;

        if (this.timeseries) {
            if (retriedInsert != 0) {
                inserted = this.retryCount;
                retriedInsert = 0;
            }
        }

        // FLE2 has 2 side collection inserts per indexedEncryptedField touched by each
        // insert/update.
        let actualInserts = inserted + 2 * (inserted + updated) * eqIndexedEncryptedFields;

        if (this.isMongos) {
            this._checkMongosOnlyMetrics(status0,
                                         status1,
                                         updateCount,
                                         inserted,
                                         deleteCount,
                                         eqIndexedEncryptedFields,
                                         singleUpdateForBulkWrite,
                                         singleInsertForBulkWrite,
                                         singleDeleteForBulkWrite,
                                         insertShardField,
                                         updateShardField,
                                         deleteShardField,
                                         fleSafeContentUpdates,
                                         retryCount,
                                         actualInserts);
        } else {
            const top1 = this.testDB.adminCommand({top: 1}).totals;
            // See comment on unshardedInsert for the ternary.
            const retriedCommandsCount =
                (1 + 2 * (eqIndexedEncryptedFields > 0 ? 1 : 0) + (this.bulkWrite && this.fle)) *
                (retryCount - 1);
            const retriedStatementsCount = (1 + 2 * eqIndexedEncryptedFields) * (retryCount - 1);
            this._checkMongodOnlyMetrics(status0,
                                         top0,
                                         status1,
                                         top1,
                                         updated,
                                         updateCount,
                                         inserted,
                                         deleted,
                                         deleteCount,
                                         retriedInsert,
                                         retriedCommandsCount,
                                         retriedStatementsCount,
                                         fleSafeContentUpdates,
                                         actualInserts,
                                         retryCount,
                                         perNamespaceMetrics,
                                         nsIndicesInRequest);
        }

        this._checkOpCounters(status0,
                              status1,
                              inserted,
                              actualInserts,
                              updateCount,
                              updateManyCount,
                              opcounterFactor,
                              fleSafeContentUpdates,
                              deleteCount,
                              deleteManyCount,
                              retryCount);
        this._checkUpdateMetrics(status0, status1, updateArrayFilters, updatePipeline);
    }

    // Add the writeConcern. If this.timeseries is true, add a timestamp field if missing.
    executeCommand(command) {
        if (this.timeseries) {
            if (command.hasOwnProperty("documents")) {
                for (let document of command.documents) {
                    if (!document.hasOwnProperty("timestamp")) {
                        document.timestamp = this.defaultTimestamp;
                    }
                }
            } else if (command.hasOwnProperty("updates")) {
                for (let document of command.updates) {
                    if (!document.q.hasOwnProperty("timestamp")) {
                        document.q.timestamp = this.defaultTimestamp;
                    }
                }
            }
        }
        command.writeConcern = {w: "majority"};
        return assert.commandWorked(this.testDB.runCommand(command));
    }

    // Adds a timestamp field if missing. Called when this.timeseries is true.
    _addTimestamp(bulkWriteOps) {
        for (let op of bulkWriteOps) {
            if (op.hasOwnProperty("delete")) {
                continue;
            }
            if (op.hasOwnProperty("document")) {
                if (!op.document.hasOwnProperty("timestamp")) {
                    op.document.timestamp = this.defaultTimestamp;
                }
            } else {
                if (!op.filter.hasOwnProperty("timestamp")) {
                    op.filter.timestamp = this.defaultTimestamp;
                }
            }
        }
    }

    _findNsIndicesInRequest(bulkWriteOps) {
        const nsIndicesInRequest = new Set();
        for (const op of bulkWriteOps) {
            var idx = op.insert;
            if (op.update != undefined) {
                idx = op.update;
            } else if (op.delete != undefined) {
                idx = op.delete;
            }

            nsIndicesInRequest.add(idx);
        }
        return nsIndicesInRequest;
    }

    checkMetrics(testcaseName, bulkWriteOps, normalCommands, expectedMetrics) {
        jsTest.log.info(`Testcase: ${testcaseName} (on a ${
            this.isMongos ? "ShardingTest"
                          : "ReplSetTest"} with bulkWrite = ${this.bulkWrite}, errorsOnly = ${
            this.errorsOnly} and timeseries = ${this.timeseries}).`);
        const statusBefore = this.testDB.serverStatus();
        const topBefore = this.isMongos ? undefined : this.testDB.adminCommand({top: 1}).totals;

        if (this.bulkWrite) {
            if (this.timeseries) {
                this._addTimestamp(bulkWriteOps);
            }

            const namespaces = this.namespaces.map(namespace => {
                return {ns: namespace};
            });

            assert.commandWorked(this.testDB.adminCommand({
                bulkWrite: 1,
                ops: bulkWriteOps,
                nsInfo: namespaces,
                writeConcern: {w: 'majority'},
                errorsOnly: this.errorsOnly
            }));
        } else {
            for (let command of normalCommands) {
                this.executeCommand(command);
            }
        }
        expectedMetrics.retryCount = 1;
        this._checkMetricsImpl(
            statusBefore, topBefore, this._findNsIndicesInRequest(bulkWriteOps), expectedMetrics);
    }

    checkMetricsWithRetries(
        testcaseName, bulkWriteOps, normalCommand, expectedMetrics, lsid, txnNumber) {
        jsTest.log.info(`Testcase: ${testcaseName} (on a ${
            this.isMongos ? "ShardingTest"
                          : "ReplSetTest"} with bulkWrite = ${this.bulkWrite}, errorsOnly = ${
            this.errorsOnly} and timeseries = ${this.timeseries}).`);

        if (this.timeseries && this.isMongos) {
            // For sharded timeseries updates we will get an extra opcounter for a retryable write
            // since we execute them as an internal transaction which does an additional opcounter.
            expectedMetrics.opcounterFactor = 2;
            if (expectedMetrics.updateArrayFilters) {
                expectedMetrics.updateArrayFilters = expectedMetrics.updateArrayFilters * 2;
            }
        }

        let statusBefore = this.testDB.serverStatus();
        let topBefore = this.isMongos ? undefined : this.testDB.adminCommand({top: 1}).totals;
        if (this.bulkWrite) {
            if (this.timeseries) {
                this._addTimestamp(bulkWriteOps);
            }

            const namespaces = this.namespaces.map(namespace => {
                return {ns: namespace};
            });

            for (let i = 0; i < this.retryCount; ++i) {
                let res = assert.commandWorked(this.testDB.adminCommand({
                    bulkWrite: 1,
                    ops: bulkWriteOps,
                    nsInfo: namespaces,
                    lsid: lsid,
                    txnNumber: txnNumber,
                    writeConcern: {w: "majority"}
                }));
                assert.eq(res.cursor.firstBatch[0].ok, 1);
            }
        } else {
            normalCommand.writeConcern = {w: "majority"};
            normalCommand.lsid = lsid;
            normalCommand.txnNumber = txnNumber;

            for (let i = 0; i < this.retryCount; ++i) {
                this.executeCommand(normalCommand);
            }
        }
        expectedMetrics.retryCount = this.retryCount;
        this._checkMetricsImpl(
            statusBefore, topBefore, this._findNsIndicesInRequest(bulkWriteOps), expectedMetrics);
    }
}
