/**
 * Overrides the runCommand method to prefix all databases and namespaces ("config", "admin",
 * "local" excluded) with a tenant prefix, so that the accessed data will be migrated by the
 * background operations run by the ContinuousTenantMigration and ContinuousShardSplit hooks.
 */
(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.
load("jstests/libs/transactions_util.js");

// Save references to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
const originalRunCommand = Mongo.prototype.runCommand;
const originalCloseMethod = Mongo.prototype.close;

// Save a reference to the connection created at shell startup. This will be used as a proxy for
// multiple internal routing connections for the lifetime of the test execution.
const initialConn = db.getMongo();

const testTenantMigrationDB = "testTenantMigration";
// For shard merge we need to use the local DB that is not blocked by tenant access blockers.
const localDB = "local";

/**
 * Asserts that the provided connection is an internal routing connection, not the top-level proxy
 * connection. The proxy connection also has an internal routing connection, so it is excluded from
 * this check.
 */
function assertRoutingConnection(conn) {
    if (conn !== initialConn) {
        assert.eq(null,
                  conn._internalRoutingConnection,
                  "Expected connection to have no internal routing connection.");
    }
}

/**
 * @returns The internal routing connection for a provided connection
 */
function getRoutingConnection(conn) {
    if (conn === initialConn && conn._internalRoutingConnection == null) {
        conn._internalRoutingConnection = conn;
    }

    // Since we are patching the prototype below, there must eventually be a "base case" for
    // determining which connection to run a method on. If the provided `conn` has no internal
    // routing connection, we assume that it _is_ the internal routing connection, and return
    // here.
    if (conn._internalRoutingConnection == null) {
        return conn;
    }

    // Sanity check ensuring we have not accidentally created an internal routing connection on an
    // internal routing connection.
    assertRoutingConnection(conn._internalRoutingConnection);
    return conn._internalRoutingConnection;
}

/**
 * Assigns a newly establish connection as the internal routing connection for a Mongo instance.
 *
 * @param {Mongo} conn The original Mongo connection
 * @param {Mongo} mongo The newly established, internal connection
 */
function setRoutingConnection(conn, mongo) {
    assert.neq(null,
               conn._internalRoutingConnection,
               "Expected connection to already have an internal routing connection.");
    conn._internalRoutingConnection = mongo;
}

function closeRoutingConnection(conn) {
    if (conn === initialConn) {
        // We need to close the initial connection differently, since we patch the close method
        // below to always proxy calls to the internal routing connection.
        return originalCloseMethod.apply(conn);
    }

    // For all other connections we are safe to call close directly
    conn.close();
}

/**
 * @returns Whether we are currently running an operation with multiple tenants.
 */
function usingMultipleTenants() {
    return !!TestData.tenantIds;
}

const kDenylistedDbNames = new Set(["config", "admin", "local"]);
function isDenylistedDb(dbName) {
    return kDenylistedDbNames.has(dbName);
}

const kTenantPrefixMap = {};

/**
 * If the database with the given name can be migrated, prepend a tenant prefix if one has not
 * already been applied.
 */
function prependTenantIdToDbNameIfApplicable(dbName) {
    if (dbName.length === 0) {
        // There are input validation tests that use invalid database names, those should be
        // ignored.
        return dbName;
    }

    let prefix;
    // If running shard split passthroughs, then assign a database to a randomly selected tenant
    if (usingMultipleTenants()) {
        if (!kTenantPrefixMap[dbName]) {
            const tenantId =
                TestData.tenantIds[Math.floor(Math.random() * TestData.tenantIds.length)];
            kTenantPrefixMap[dbName] = `${tenantId}_`;
        }

        prefix = kTenantPrefixMap[dbName];
    } else {
        prefix = `${TestData.tenantId}_`;
    }

    return (isDenylistedDb(dbName) || dbName.startsWith(prefix)) ? dbName : `${prefix}${dbName}`;
}

/**
 * If the database for the given namespace can be migrated, prepend a tenant prefix if one has not
 * already been applied.
 */
function prependTenantIdToNsIfApplicable(ns) {
    if (ns.length === 0 || !ns.includes(".")) {
        // There are input validation tests that use invalid namespaces, those should be ignored.
        return ns;
    }

    const splitNs = ns.split(".");
    splitNs[0] = prependTenantIdToDbNameIfApplicable(splitNs[0]);
    return splitNs.join(".");
}

/**
 * Remove a tenant prefix from the provided database name, if applicable.
 */
function extractOriginalDbName(dbName) {
    if (usingMultipleTenants()) {
        const anyTenantPrefixOnceRegex = new RegExp(Object.values(kTenantPrefixMap).join('|'), '');
        return dbName.replace(anyTenantPrefixOnceRegex, "");
    }

    return dbName.replace(`${TestData.tenantId}_`, "");
}

/**
 * Remove a tenant prefix from the provided namespace, if applicable.
 */
function extractOriginalNs(ns) {
    const splitNs = ns.split(".");
    splitNs[0] = extractOriginalDbName(splitNs[0]);
    return splitNs.join(".");
}

/**
 * Removes all occurrences of a tenant prefix in the provided string.
 */
function removeTenantIdFromString(string) {
    if (usingMultipleTenants()) {
        const anyTenantPrefixGlobalRegex =
            new RegExp(Object.values(kTenantPrefixMap).join('|'), 'g');
        return string.replace(anyTenantPrefixGlobalRegex, "");
    }

    return string.replace(new RegExp(`${TestData.tenantId}_`, 'g'), "");
}

/**
 * @returns Whether we are running a shard split passthrough.
 */
function isShardSplitPassthrough() {
    return !!TestData.splitPassthrough;
}

/**
 * Prepends a tenant prefix to all database name and namespace fields in the provided object, where
 * applicable.
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
 * Removes a tenant prefix from all the database name and namespace fields in the provided object,
 * where applicable.
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
 * Returns true if the provided command object has had a tenant prefix appended to its namespaces.
 */
function isCmdObjWithTenantId(cmdObj) {
    return cmdObj.comment && cmdObj.comment.isCmdObjWithTenantId;
}

/**
 * Prepend a tenant prefix to all namespaces within a provided command object, and record a comment
 * indicating that the command object has alrady been modified.
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

    // BulkWrite command has errors contained in a cursor response. The error will always be
    // in the first batch of the cursor response since getMore is not allowed to run with
    // tenant migration / shard merge suites.
    if (resObj.cursor) {
        if (resObj.cursor.firstBatch) {
            for (let opRes of resObj.cursor.firstBatch) {
                if (opRes.code && opRes.code == errorCode) {
                    return {code: opRes.code, errmsg: opRes.errmsg};
                }
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

    if (cmdObj.bulkWrite) {
        let retryOps = [];
        // For bulkWrite tenant migration errors always act as if they are executed as
        // `ordered:true` meaning we will have to retry every op from the one that errored.
        retryOps =
            cmdObj.ops.slice(resObj.cursor.firstBatch[resObj.cursor.firstBatch.length - 1].idx);
        cmdObj.ops = retryOps;
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
 * Rewrites a server connection string (ex: rsName/host,host,host) to a URI that the shell can
 * connect to.
 */
function convertServerConnectionStringToURI(input) {
    const inputParts = input.split('/');
    return `mongodb://${inputParts[1]}/?replicaSet=${inputParts[0]}`;
}

/**
 * Returns the state document for the outgoing tenant migration or shard split operation. Asserts
 * that there is only one such operation.
 */
function getOperationStateDocument(conn) {
    const collection = isShardSplitPassthrough() ? "shardSplitDonors" : "tenantMigrationDonors";
    let filter = {tenantId: TestData.tenantId};
    if (usingMultipleTenants()) {
        let tenantIds = [];
        TestData.tenantIds.forEach(tenantId => tenantIds.push(ObjectId(tenantId)));
        filter = {tenantIds: tenantIds};
    }

    const findRes = assert.commandWorked(
        originalRunCommand.apply(conn, ["config", {find: collection, filter}, 0]));

    const docs = findRes.cursor.firstBatch;
    // There should only be one active migration at any given time.
    assert.eq(docs.length, 1, tojson(docs));

    const result = docs[0];
    if (isShardSplitPassthrough()) {
        result.recipientConnectionString =
            convertServerConnectionStringToURI(result.recipientConnectionString);
    }

    return result;
}

/**
 * Marks the outgoing tenant migration or shard split operation as having caused the shell to
 * reroute commands by inserting a document for it into the testTenantMigration.rerouted collection
 * or local.rerouted collection for the shard merge protocol.
 */
function recordRerouteDueToTenantMigration(conn, migrationStateDoc) {
    assertRoutingConnection(conn);
    const dbToCheck = TestData.useLocalDBForDBCheck ? localDB : testTenantMigrationDB;
    while (true) {
        try {
            const res = originalRunCommand.apply(conn, [
                dbToCheck,
                {
                    insert: "rerouted",
                    documents: [{_id: migrationStateDoc._id}],
                    writeConcern: {w: "majority"}
                },
                0
            ]);

            if (res.ok) {
                break;
            } else if (isRetryableError(res)) {
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
            if (isRetryableError(e)) {
                jsTest.log(
                    "Failed to write to testTenantMigration.rerouted due to a retryable error exception " +
                    tojson(e));
                continue;
            }
            throw e;
        }
    }
}

/**
 * Keeps executing 'cmdObjWithTenantId' by running 'originalRunCommandFunc' if 'this.reroutingMongo'
 * is not none or 'reroutingRunCommandFunc' if it is none until the command succeeds or fails with
 * errors other than TenantMigrationCommitted or TenantMigrationAborted. When the command fails
 * with TenantMigrationCommitted, sets 'this.reroutingMongo' to the mongo connection to the
 * recipient for the migration. 'dbNameWithTenantId' is only used for logging.
 */
function runCommandRetryOnTenantMigrationErrors(
    conn, dbNameWithTenantId, cmdObjWithTenantId, options) {
    let numAttempts = 0;

    // Keep track of the write operations that were applied.
    let n = 0;
    let nModified = 0;
    let upserted = [];
    let nonRetryableWriteErrors = [];
    let bulkWriteResponse = {};
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
        const newConn = getRoutingConnection(conn);
        let resObj =
            originalRunCommand.apply(newConn, [dbNameWithTenantId, cmdObjWithTenantId, options]);

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
            // bulkWrite case.
            if (cmdObjWithTenantId.bulkWrite) {
                // First attempt store the whole response.
                if (numAttempts == 1) {
                    bulkWriteResponse = resObj;
                } else {
                    // The last item from the previous response is guaranteed to be a
                    // tenant migration error. Remove it to append the retried response.
                    let newIdx = bulkWriteResponse.cursor.firstBatch.pop().idx;
                    // Iterate over new response and change the indexes to start with newIdx.
                    for (let opRes of resObj.cursor.firstBatch) {
                        opRes.idx = newIdx;
                        newIdx += 1;
                    }

                    // Add the new responses (with modified indexes) onto the original responses.
                    bulkWriteResponse.cursor.firstBatch =
                        bulkWriteResponse.cursor.firstBatch.concat(resObj.cursor.firstBatch);

                    // Add new numErrors onto old numErrors. Subtract one to account for the
                    // tenant migration error that was popped off.
                    bulkWriteResponse.numErrors += resObj.numErrors - 1;
                }
            }

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
                const donorConnection = getRoutingConnection(conn);
                const migrationStateDoc = getOperationStateDocument(donorConnection);
                setRoutingConnection(
                    conn, connect(migrationStateDoc.recipientConnectionString).getMongo());

                // After getting a TenantMigrationCommitted error, wait for the python test fixture
                // to do a dbhash check on the donor and recipient primaries before we retry the
                // command on the recipient.
                const dbToCheck = TestData.useLocalDBForDBCheck ? localDB : testTenantMigrationDB;
                assert.soon(() => {
                    let findRes = assert.commandWorked(originalRunCommand.apply(donorConnection, [
                        dbToCheck,
                        {
                            find: "dbhashCheck",
                            filter: {_id: migrationStateDoc._id},
                        },
                        0
                    ]));

                    const docs = findRes.cursor.firstBatch;
                    return docs[0] != null;
                });

                recordRerouteDueToTenantMigration(donorConnection, migrationStateDoc);

                if (isShardSplitPassthrough()) {
                    closeRoutingConnection(donorConnection);
                }
            } else if (migrationAbortedErr) {
                jsTestLog(`Got TenantMigrationAborted for command against database ${
                              dbNameWithTenantId} after trying ${numAttempts} times: ` +
                          `${tojson(cmdObjWithTenantId)} -> ${tojson(resObj)}`);
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
                if (cmdObjWithTenantId.bulkWrite) {
                    resObj = bulkWriteResponse;
                }
            }
            return resObj;
        }
    }
}

Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    const dbNameWithTenantId = prependTenantIdToDbNameIfApplicable(dbName);

    // Prepend a tenant prefix to all database names and namespaces, where applicable.
    const originalCmdObjContainsTenantId = isCmdObjWithTenantId(cmdObj);
    const cmdObjWithTenantId =
        originalCmdObjContainsTenantId ? cmdObj : createCmdObjWithTenantId(cmdObj);

    const resObj = runCommandRetryOnTenantMigrationErrors(
        this, dbNameWithTenantId, cmdObjWithTenantId, options);

    if (!originalCmdObjContainsTenantId) {
        // Remove the tenant prefix from all database names and namespaces in the result since tests
        // assume the command was run against the original database.
        removeTenantId(resObj);
    }

    return resObj;
};

// Override all base methods on the Mongo prototype to try to proxy the call to the underlying
// internal routing connection, if one exists.
// NOTE: This list is derived from scripting/mozjs/mongo.cpp:62.
['auth',
 'close',
 'compact',
 'cursorHandleFromId',
 'find',
 'generateDataKey',
 'getDataKeyCollection',
 'logout',
 'encrypt',
 'decrypt',
 'isReplicaSetConnection',
 '_markNodeAsFailed',
 'getMinWireVersion',
 'getMaxWireVersion',
 'isReplicaSetMember',
 'isMongos',
 'isTLS',
 'getApiParameters',
 '_startSession',
 '_refreshAccessToken',
 // Don't override this method, since it is never called directly in jstests. The expectation of is
 // that it will be run on the connection `Mongo.prototype.runCommand` chose.
 // '_runCommandImpl',
].forEach(methodName => {
    const $method = Mongo.prototype[methodName];
    Mongo.prototype[methodName] = function() {
        return $method.apply(getRoutingConnection(this), arguments);
    };
});

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/inject_tenant_prefix.js");
}());
