/**
 * Overrides the database name of each accessed database ("config", "admin", "local" excluded) to
 * have the prefix TestData.tenantId so that the accessed data will be migrated by the background
 * tenant migrations run by the ContinuousTenantMigration hook.
 */
(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.
load("jstests/libs/transactions_util.js");

// Save references to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
let originalRunCommand = Mongo.prototype.runCommand;
let originalRunCommandWithMetadata = Mongo.prototype.runCommandWithMetadata;
let originalMarkNodeAsFailed = Mongo.prototype._markNodeAsFailed;

const denylistedDbNames = ["config", "admin", "local"];

function isDenylistedDb(dbName) {
    return denylistedDbNames.includes(dbName);
}

/**
 * If the database with the given name can be migrated, prepends TestData.tenantId to the name if
 * it does not already start with the prefix.
 */
function prependTenantIdToDbNameIfApplicable(dbName) {
    if (dbName.length === 0) {
        // There are input validation tests that use invalid database names, those should be
        // ignored.
        return dbName;
    }
    const prefix = TestData.tenantId + "_";
    return isDenylistedDb(dbName) || dbName.startsWith(prefix) ? dbName : prefix + dbName;
}

/**
 * If the database for the given namespace can be migrated, prepends TestData.tenantId to the
 * namespace if it does not already start with the prefix.
 */
function prependTenantIdToNsIfApplicable(ns) {
    if (ns.length === 0 || !ns.includes(".")) {
        // There are input validation tests that use invalid namespaces, those should be ignored.
        return ns;
    }
    let splitNs = ns.split(".");
    splitNs[0] = prependTenantIdToDbNameIfApplicable(splitNs[0]);
    return splitNs.join(".");
}

/**
 * If the given database name starts TestData.tenantId, removes the prefix.
 */
function extractOriginalDbName(dbName) {
    return dbName.replace(TestData.tenantId + "_", "");
}

/**
 * If the database name for the given namespace starts TestData.tenantId, removes the prefix.
 */
function extractOriginalNs(ns) {
    let splitNs = ns.split(".");
    splitNs[0] = extractOriginalDbName(splitNs[0]);
    return splitNs.join(".");
}

/**
 * Removes all occurrences of TestDatabase.tenantId in the string.
 */
function removeTenantIdFromString(string) {
    return string.replace(new RegExp(TestData.tenantId + "_", "g"), "");
}

/**
 * Prepends TestDatabase.tenantId to all the database name and namespace fields inside the given
 * object.
 */
function prependTenantId(obj) {
    for (let k of Object.keys(obj)) {
        let v = obj[k];
        if (typeof v === "string") {
            if (k === "dbName" || k == "db") {
                obj[k] = prependTenantIdToDbNameIfApplicable(v);
            } else if (k === "namespace" || k === "ns") {
                obj[k] = prependTenantIdToNsIfApplicable(v);
            }
        } else if (Array.isArray(v)) {
            obj[k] = v.map((item) => {
                return (typeof item === "object" && item !== null) ? prependTenantId(item) : item;
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            obj[k] = prependTenantId(v);
        }
    }
    return obj;
}

/**
 * Removes TestDatabase.tenantId from all the database name and namespace fields inside the given
 * object.
 */
function removeTenantId(obj) {
    for (let k of Object.keys(obj)) {
        let v = obj[k];
        let originalK = removeTenantIdFromString(k);
        if (typeof v === "string") {
            if (k === "dbName" || k == "db" || k == "dropped") {
                obj[originalK] = extractOriginalDbName(v);
            } else if (k === "namespace" || k === "ns") {
                obj[originalK] = extractOriginalNs(v);
            } else if (k === "errmsg" || k == "name") {
                obj[originalK] = removeTenantIdFromString(v);
            }
        } else if (Array.isArray(v)) {
            obj[originalK] = v.map((item) => {
                return (typeof item === "object" && item !== null) ? removeTenantId(item) : item;
            });
        } else if (typeof v === "object" && v !== null && Object.keys(v).length > 0) {
            obj[originalK] = removeTenantId(v);
        }
    }
    return obj;
}

const kCmdsWithNsAsFirstField =
    new Set(["renameCollection", "checkShardingIndex", "dataSize", "datasize", "splitVector"]);

/**
 * Returns true if cmdObj has been marked as having TestData.tenantId prepended to all of its
 * database name and namespace fields, and false otherwise. Assumes that 'createCmdObjWithTenantId'
 * sets cmdObj.comment.isCmdObjWithTenantId to true.
 */
function isCmdObjWithTenantId(cmdObj) {
    return cmdObj.comment && cmdObj.comment.isCmdObjWithTenantId;
}

/**
 * If cmdObj.comment.isCmdObjWithTenantId is false, returns a new cmdObj with TestData.tenantId
 * prepended to all of its database name and namespace fields, and sets the flag to true after doing
 * so. Otherwise, returns an exact copy of cmdObj.
 */
function createCmdObjWithTenantId(cmdObj) {
    const cmdName = Object.keys(cmdObj)[0];
    let cmdObjWithTenantId = TransactionsUtil.deepCopyObject({}, cmdObj);

    if (isCmdObjWithTenantId(cmdObj)) {
        return cmdObjWithTenantId;
    }

    // Handle commands with special database and namespace field names.
    if (kCmdsWithNsAsFirstField.has(cmdName)) {
        cmdObjWithTenantId[cmdName] = prependTenantIdToNsIfApplicable(cmdObjWithTenantId[cmdName]);
    }

    switch (cmdName) {
        case "renameCollection":
            cmdObjWithTenantId.to = prependTenantIdToNsIfApplicable(cmdObjWithTenantId.to);
            break;
        case "internalRenameIfOptionsAndIndexesMatch":
            cmdObjWithTenantId.from = prependTenantIdToNsIfApplicable(cmdObjWithTenantId.from);
            cmdObjWithTenantId.to = prependTenantIdToNsIfApplicable(cmdObjWithTenantId.to);
            break;
        case "configureFailPoint":
            if (cmdObjWithTenantId.data) {
                if (cmdObjWithTenantId.data.namespace) {
                    cmdObjWithTenantId.data.namespace =
                        prependTenantIdToNsIfApplicable(cmdObjWithTenantId.data.namespace);
                } else if (cmdObjWithTenantId.data.ns) {
                    cmdObjWithTenantId.data.ns =
                        prependTenantIdToNsIfApplicable(cmdObjWithTenantId.data.ns);
                }
            }
            break;
        case "applyOps":
            for (let op of cmdObjWithTenantId.applyOps) {
                if (typeof op.ns === "string" && op.ns.endsWith("system.views") && op.o._id &&
                    typeof op.o._id === "string") {
                    // For views, op.ns and op.o._id must be equal.
                    op.o._id = prependTenantIdToNsIfApplicable(op.o._id);
                }
            }
            break;
        default:
            break;
    }

    // Recursively override the database name and namespace fields. Exclude 'configureFailPoint'
    // since data.errorExtraInfo.namespace or data.errorExtraInfo.ns can sometimes refer to
    // collection name instead of namespace.
    if (cmdName != "configureFailPoint") {
        prependTenantId(cmdObjWithTenantId);
    }

    cmdObjWithTenantId.comment = Object.assign(
        cmdObjWithTenantId.comment ? cmdObjWithTenantId.comment : {}, {isCmdObjWithTenantId: true});
    return cmdObjWithTenantId;
}

/**
 * If the given response object contains the given tenant migration error, returns the error object.
 * Otherwise, returns null.
 */
function extractTenantMigrationError(resObj, errorCode) {
    if (resObj.code == errorCode) {
        // Commands, like createIndex and dropIndex, have TenantMigrationCommitted or
        // TenantMigrationAborted error in the top level.
        return resObj;
    }
    if (resObj.writeErrors) {
        for (let writeError of resObj.writeErrors) {
            if (writeError.code == errorCode) {
                return writeError;
            }
        }
    }
    return null;
}

/**
 * If the response contains the 'writeErrors' field, replaces it with a field named
 * 'truncatedWriteErrors' which includes only the first and last error object in 'writeErrors'.
 * To be used for logging.
 */
function reformatResObjForLogging(resObj) {
    if (resObj.writeErrors) {
        let truncatedWriteErrors = [resObj.writeErrors[0]];
        if (resObj.writeErrors.length > 1) {
            truncatedWriteErrors.push(resObj.writeErrors[resObj.writeErrors.length - 1]);
        }
        resObj["truncatedWriteErrors"] = truncatedWriteErrors;
        delete resObj.writeErrors;
    }
}

/**
 * If the command was a batch command where some of the operations failed, modifies the command
 * object so that only failed operations are retried.
 */
function modifyCmdObjForRetry(cmdObj, resObj) {
    if (!resObj.hasOwnProperty("writeErrors") && ErrorCodes.isTenantMigrationError(resObj.code)) {
        // If we get a top level error without writeErrors, retry the entire command.
        return;
    }

    if (cmdObj.insert) {
        let retryOps = [];
        if (cmdObj.ordered === false) {
            for (let writeError of resObj.writeErrors) {
                if (ErrorCodes.isTenantMigrationError(writeError.code)) {
                    retryOps.push(cmdObj.documents[writeError.index]);
                }
            }
        } else {
            retryOps = cmdObj.documents.slice(resObj.writeErrors[0].index);
        }
        cmdObj.documents = retryOps;
    }

    // findAndModify may also have an update field, but is not a batched command.
    if (cmdObj.update && !cmdObj.findAndModify && !cmdObj.findandmodify) {
        let retryOps = [];
        if (cmdObj.ordered === false) {
            for (let writeError of resObj.writeErrors) {
                if (ErrorCodes.isTenantMigrationError(writeError.code)) {
                    retryOps.push(cmdObj.updates[writeError.index]);
                }
            }
        } else {
            retryOps = cmdObj.updates.slice(resObj.writeErrors[0].index);
        }
        cmdObj.updates = retryOps;
    }

    if (cmdObj.delete) {
        let retryOps = [];
        if (cmdObj.ordered === false) {
            for (let writeError of resObj.writeErrors) {
                if (ErrorCodes.isTenantMigrationError(writeError.code)) {
                    retryOps.push(cmdObj.deletes[writeError.index]);
                }
            }
        } else {
            retryOps = cmdObj.deletes.slice(resObj.writeErrors[0].index);
        }
        cmdObj.deletes = retryOps;
    }
}

/**
 * Sets the keys of the given index map to consecutive non-negative integers starting from 0.
 */
function resetIndices(indexMap) {
    let newIndexMap = {};
    Object.keys(indexMap).map((key, index) => {
        newIndexMap[index] = indexMap[key];
    });
    return newIndexMap;
}

function toIndexSet(indexedDocs) {
    let set = new Set();
    if (indexedDocs) {
        for (let doc of indexedDocs) {
            set.add(doc.index);
        }
    }
    return set;
}

/**
 * Remove the indices for non-upsert writes that succeeded.
 */
function removeSuccessfulOpIndexesExceptForUpserted(resObj, indexMap, ordered) {
    // Optimization to only look through the indices in a set rather than in an array.
    let indexSetForUpserted = toIndexSet(resObj.upserted);
    let indexSetForWriteErrors = toIndexSet(resObj.writeErrors);

    for (let index in Object.keys(indexMap)) {
        if ((!indexSetForUpserted.has(parseInt(index)) &&
             !(ordered && resObj.writeErrors && (index > resObj.writeErrors[0].index)) &&
             !indexSetForWriteErrors.has(parseInt(index)))) {
            delete indexMap[index];
        }
    }
    return indexMap;
}

/**
 * Returns the state document for the outgoing tenant migration for TestData.tenantId. Asserts
 * that there is only one such migration.
 */
Mongo.prototype.getTenantMigrationStateDoc = function() {
    const findRes = assert.commandWorked(originalRunCommand.apply(
        this,
        ["config", {find: "tenantMigrationDonors", filter: {tenantId: TestData.tenantId}}, 0]));
    const docs = findRes.cursor.firstBatch;
    // There should only be one active migration at any given time.
    assert.eq(docs.length, 1, tojson(docs));
    return docs[0];
};

/**
 * Marks the outgoing migration for TestData.tenantId as having caused the shell to reroute
 * commands by inserting a document for it into the testTenantMigration.rerouted collection.
 */
Mongo.prototype.recordRerouteDueToTenantMigration = function() {
    assert.neq(null, this.migrationStateDoc);
    assert.neq(null, this.reroutingMongo);

    while (true) {
        try {
            const res = originalRunCommand.apply(this, [
                "testTenantMigration",
                {
                    insert: "rerouted",
                    documents: [{_id: this.migrationStateDoc._id}],
                    writeConcern: {w: "majority"}
                },
                0
            ]);

            if (res.ok) {
                break;
            } else if (ErrorCodes.isNetworkError(res.code) ||
                       ErrorCodes.isNotPrimaryError(res.code)) {
                jsTest.log(
                    "Failed to write to testTenantMigration.rerouted due to a retryable error " +
                    tojson(res));
                continue;
            } else {
                // Throw non-retryable errors.
                assert.commandWorked(res);
            }
        } catch (e) {
            // Since the shell can throw custom errors that don't propagate the error code, check
            // these exceptions for specific network error messages.
            // TODO SERVER-54026: Remove check for network error messages once the shell reliably
            // returns error codes.
            if (ErrorCodes.isNetworkError(e.code) || ErrorCodes.isNotPrimaryError(e.code) ||
                isNetworkError(e)) {
                jsTest.log(
                    "Failed to write to testTenantMigration.rerouted due to a retryable error exception " +
                    tojson(e));
                continue;
            }
            throw e;
        }
    }
};

/**
 * Keeps executing 'cmdObjWithTenantId' by running 'originalRunCommandFunc' if 'this.reroutingMongo'
 * is not none or 'reroutingRunCommandFunc' if it is none until the command succeeds or fails with
 * errors other than TenantMigrationCommitted or TenantMigrationAborted. When the command fails
 * with TenantMigrationCommitted, sets 'this.reroutingMongo' to the mongo connection to the
 * recipient for the migration. 'dbNameWithTenantId' is only used for logging.
 */
Mongo.prototype.runCommandRetryOnTenantMigrationErrors = function(
    dbNameWithTenantId, cmdObjWithTenantId, originalRunCommandFunc, reroutingRunCommandFunc) {
    if (this.reroutingMongo) {
        return reroutingRunCommandFunc();
    }

    let numAttempts = 0;

    // Keep track of the write operations that were applied.
    let n = 0;
    let nModified = 0;
    let upserted = [];
    let nonRetryableWriteErrors = [];
    const isRetryableWrite =
        cmdObjWithTenantId.txnNumber && !cmdObjWithTenantId.hasOwnProperty("autocommit");

    // 'indexMap' is a mapping from a write's index in the current cmdObj to its index in the
    // original cmdObj.
    let indexMap = {};
    if (cmdObjWithTenantId.documents) {
        for (let i = 0; i < cmdObjWithTenantId.documents.length; i++) {
            indexMap[i] = i;
        }
    }
    if (cmdObjWithTenantId.updates) {
        for (let i = 0; i < cmdObjWithTenantId.updates.length; i++) {
            indexMap[i] = i;
        }
    }
    if (cmdObjWithTenantId.deletes) {
        for (let i = 0; i < cmdObjWithTenantId.deletes.length; i++) {
            indexMap[i] = i;
        }
    }

    while (true) {
        numAttempts++;
        let resObj;
        if (this.reroutingMongo) {
            this.recordRerouteDueToTenantMigration();
            resObj = reroutingRunCommandFunc();
        } else {
            resObj = originalRunCommandFunc();
        }

        const migrationCommittedErr =
            extractTenantMigrationError(resObj, ErrorCodes.TenantMigrationCommitted);
        const migrationAbortedErr =
            extractTenantMigrationError(resObj, ErrorCodes.TenantMigrationAborted);

        // If the write didn't encounter a TenantMigrationCommitted or TenantMigrationAborted error
        // at all, return the result directly.
        if (numAttempts == 1 && (!migrationCommittedErr && !migrationAbortedErr)) {
            return resObj;
        }

        // Add/modify the shells's n, nModified, upserted, and writeErrors, unless this command is
        // part of a retryable write.
        if (!isRetryableWrite) {
            if (resObj.n) {
                n += resObj.n;
            }
            if (resObj.nModified) {
                nModified += resObj.nModified;
            }
            if (resObj.upserted || resObj.writeErrors) {
                // This is an optimization to make later lookups into 'indexMap' faster, since it
                // removes any key that is not pertinent in the current cmdObj execution.
                indexMap = removeSuccessfulOpIndexesExceptForUpserted(
                    resObj, indexMap, cmdObjWithTenantId.ordered);

                if (resObj.upserted) {
                    for (let upsert of resObj.upserted) {
                        let currentUpsertedIndex = upsert.index;

                        // Set the entry's index to the write's index in the original cmdObj.
                        upsert.index = indexMap[upsert.index];

                        // Track that this write resulted in an upsert.
                        upserted.push(upsert);

                        // This write will not need to be retried, so remove it from 'indexMap'.
                        delete indexMap[currentUpsertedIndex];
                    }
                }
                if (resObj.writeErrors) {
                    for (let writeError of resObj.writeErrors) {
                        // If we encounter a TenantMigrationCommitted or TenantMigrationAborted
                        // error, the rest of the batch must have failed with the same code.
                        if (ErrorCodes.isTenantMigrationError(writeError.code)) {
                            break;
                        }

                        let currentWriteErrorIndex = writeError.index;

                        // Set the entry's index to the write's index in the original cmdObj.
                        writeError.index = indexMap[writeError.index];

                        // Track that this write resulted in a non-retryable error.
                        nonRetryableWriteErrors.push(writeError);

                        // This write will not need to be retried, so remove it from 'indexMap'.
                        delete indexMap[currentWriteErrorIndex];
                    }
                }
            }
        }

        if (migrationCommittedErr || migrationAbortedErr) {
            // If the command was inside a transaction, skip modifying any objects or fields, since
            // we will retry the entire transaction outside of this file.
            if (!TransactionsUtil.isTransientTransactionError(resObj)) {
                // Update the command for reroute/retry.
                // In the case of retryable writes, we should always retry the entire batch of
                // operations instead of modifying the original command object to only include
                // failed writes.
                if (!isRetryableWrite) {
                    modifyCmdObjForRetry(cmdObjWithTenantId, resObj, true);
                }

                // It is safe to reformat this resObj since it will not be returned to the caller of
                // runCommand.
                reformatResObjForLogging(resObj);

                // Build a new indexMap where the keys are the index that each write that needs to
                // be retried will have in the next attempt's cmdObj.
                indexMap = resetIndices(indexMap);
            }

            if (migrationCommittedErr) {
                jsTestLog(`Got TenantMigrationCommitted for command against database ${
                    dbNameWithTenantId} after trying ${numAttempts} times: ${tojson(resObj)}`);
                // Store the connection to the recipient so the next commands can be rerouted.
                this.migrationStateDoc = this.getTenantMigrationStateDoc();
                this.reroutingMongo =
                    connect(this.migrationStateDoc.recipientConnectionString).getMongo();

                // After getting a TenantMigrationCommitted error, wait for the python test fixture
                // to do a dbhash check on the donor and recipient primaries before we retry the
                // command on the recipient.
                if (!TestData.skipTenantMigrationDBHash) {
                    assert.soon(() => {
                        let findRes = assert.commandWorked(originalRunCommand.apply(this, [
                            "testTenantMigration",
                            {
                                find: "dbhashCheck",
                                filter: {_id: this.migrationStateDoc._id},
                            },
                            0
                        ]));

                        const docs = findRes.cursor.firstBatch;
                        return docs[0] != null;
                    });
                }
            } else if (migrationAbortedErr) {
                jsTestLog(`Got TenantMigrationAborted for command against database ${
                    dbNameWithTenantId} after trying ${numAttempts} times: ${tojson(resObj)}`);
            }

            // If the result has a TransientTransactionError label, the entire transaction must be
            // retried. Return immediately to let the retry be handled by
            // 'network_error_and_txn_override.js'.
            if (TransactionsUtil.isTransientTransactionError(resObj)) {
                jsTestLog(`Got error for transaction against database ` +
                          `${dbNameWithTenantId} with TransientTransactionError, retrying ` +
                          `transaction against recipient: ${tojson(resObj)}`);
                return resObj;
            }
        } else {
            if (!isRetryableWrite) {
                // Modify the resObj before returning the result.
                if (resObj.n) {
                    resObj.n = n;
                }
                if (resObj.nModified) {
                    resObj.nModified = nModified;
                }
                if (upserted.length > 0) {
                    resObj.upserted = upserted;
                }
                if (nonRetryableWriteErrors.length > 0) {
                    resObj.writeErrors = nonRetryableWriteErrors;
                }
            }
            return resObj;
        }
    }
};

Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    const dbNameWithTenantId = prependTenantIdToDbNameIfApplicable(dbName);

    // Use cmdObj with TestData.tenantId prepended to all the applicable database names and
    // namespaces.
    const originalCmdObjContainsTenantId = isCmdObjWithTenantId(cmdObj);
    let cmdObjWithTenantId =
        originalCmdObjContainsTenantId ? cmdObj : createCmdObjWithTenantId(cmdObj);

    let resObj = this.runCommandRetryOnTenantMigrationErrors(
        dbNameWithTenantId,
        cmdObjWithTenantId,
        () => originalRunCommand.apply(this, [dbNameWithTenantId, cmdObjWithTenantId, options]),
        () => this.reroutingMongo.runCommand(dbNameWithTenantId, cmdObjWithTenantId, options));

    if (!originalCmdObjContainsTenantId) {
        // Remove TestData.tenantId from all database names and namespaces in the resObj since tests
        // assume the command was run against the original database.
        removeTenantId(resObj);
    }

    Mongo.prototype._markNodeAsFailed = function(hostName, errorCode, errorReason) {
        if (this.reroutingMongo)
            originalMarkNodeAsFailed.apply(this.reroutingMongo, [hostName, errorCode, errorReason]);
        else
            originalMarkNodeAsFailed.apply(this, [hostName, errorCode, errorReason]);
    };

    return resObj;
};

Mongo.prototype.runCommandWithMetadata = function(dbName, metadata, cmdObj) {
    const dbNameWithTenantId = prependTenantIdToDbNameIfApplicable(dbName);

    // Use cmdObj with TestData.tenantId prepended to all the applicable database names and
    // namespaces.
    const originalCmdObjContainsTenantId = isCmdObjWithTenantId(cmdObj);
    let cmdObjWithTenantId =
        originalCmdObjContainsTenantId ? cmdObj : createCmdObjWithTenantId(cmdObj);

    let resObj = this.runCommandRetryOnTenantMigrationErrors(
        dbNameWithTenantId,
        cmdObjWithTenantId,
        () => originalRunCommandWithMetadata.apply(
            this, [dbNameWithTenantId, metadata, cmdObjWithTenantId]),
        () => this.reroutingMongo.runCommandWithMetadata(
            dbNameWithTenantId, metadata, cmdObjWithTenantId));

    if (!originalCmdObjContainsTenantId) {
        // Remove TestData.tenantId from all database names and namespaces in the resObj since tests
        // assume the command was run against the original database.
        removeTenantId(resObj);
    }

    return resObj;
};

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/inject_tenant_prefix.js");
}());
