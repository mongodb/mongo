/**
 * Overrides the runCommand method to convert specified CRUD ops into bulkWrite commands.
 * Converts the bulkWrite responses into the original CRUD response.
 */
import {BulkWriteUtils} from "jstests/libs/crud_ops_to_bulk_write_lib.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

let normalCluster = connect(TestData.normalCluster).getMongo();
let bulkWriteCluster = connect(TestData.bulkWriteCluster).getMongo();

let normalClusterRunCommand = TestData.preOverrideRunCommand;

jsTestLog("Normal Cluster: " + normalCluster);
jsTestLog("BulkWrite Cluster: " + bulkWriteCluster);

const errorsOnly = Math.random() < 0.75;

jsTestLog("Running bulkWrite override with `errorsOnly:" + errorsOnly + "`");

const normalSession = normalCluster.getDB("admin")._session;
const normalSessionId = normalSession._serverSession.handle.getId();

const bulkSession = bulkWriteCluster.getDB("admin")._session;
const bulkSessionId = bulkSession._serverSession.handle.getId();

const maxBatchSize = 5;

function getLetFromCommand(cmdObj) {
    if (cmdObj.hasOwnProperty("updates")) {
        if (cmdObj.updates[0].hasOwnProperty("let")) {
            return cmdObj.updates[0].let;
        }
    } else if (cmdObj.hasOwnProperty("deletes")) {
        if (cmdObj.deletes[0].hasOwnProperty("let")) {
            return cmdObj.updates[0].let;
        }
    } else if (cmdObj.hasOwnProperty("let")) {
        return cmdObj.let;
    }
    return null;
}

function opCompatibleWithCurrentBatch(dbName, collName, cmdObj) {
    if (BulkWriteUtils.getCurrentBatchSize() >= maxBatchSize) {
        return false;
    }

    let state = BulkWriteUtils.getBulkWriteState();

    // Check if namespace exists and the stored collectionUUID / encryptionInfo is different
    let idx = state.nsInfos.findIndex((element) => element.ns == dbName + "." + collName);

    if (idx != -1) {
        let nsInfo = state.nsInfos[idx];
        if (cmdObj.collectionUUID !== nsInfo.collectionUUID) {
            return false;
        }
        if (cmdObj.encryptionInformation !== nsInfo.encryptionInformation) {
            return false;
        }
    }

    const bypassDocumentValidation = state.bypassDocumentValidation;

    // If bypassDocumentValidation is not set we can continue. If the stored
    // bypassDocumentValidation and the command bypassDocumentValidation are the same we can
    // continue.
    let cmdBypassDocumentValidation = cmdObj.hasOwnProperty("bypassDocumentValidation") &&
        (cmdObj.bypassDocumentValidation == true);
    if (bypassDocumentValidation != null &&
        (cmdBypassDocumentValidation != bypassDocumentValidation)) {
        return false;
    }

    const currentCmdLet = getLetFromCommand(cmdObj);
    const letObj = state.letObj;

    // If 'letObj' is null then we can always continue. If 'letObj' is not null and cmdObj.let is
    // then we can always continue. If both objects are not null and they are the same we can
    // continue.
    if (letObj != null && currentCmdLet != null && 0 === bsonWoCompare(letObj, currentCmdLet)) {
        return false;
    }

    const ordered = state.ordered;
    // If saved ordered is false or the incoming ordered is false we must flush the batch.
    let newOrdered = cmdObj.hasOwnProperty("ordered") ? cmdObj.ordered : true;
    if (!ordered || !newOrdered) {
        return false;
    }

    const newRawData = cmdObj.hasOwnProperty("rawData") ? cmdObj.rawData : false;
    const currentRawData = state.rawData;
    if (currentRawData != newRawData) {
        return false;
    }

    return true;
}

function validateClusterConsistency(originalRunCommand, makeRunCommandArgs) {
    // Replace runCommand temporarily to avoid getMores from looping back into this override from
    // the DataConsistencyChecker.
    let newRunCommand = Mongo.prototype.runCommand;
    Mongo.prototype.runCommand = normalClusterRunCommand;

    // Want to check that every namespace we just altered is the same on both clusters.
    BulkWriteUtils.getNamespaces().forEach(nsInfo => {
        let [dbName, ...coll] = nsInfo.ns.split('.');
        coll = coll.join('.');

        // Using originalRunCommand directly to avoid recursing back into this override file.
        // Need to provide session to the find command since it is automatically applied
        // to getMore by DBCommandCursor.

        // We use a very large batch size in this find command because getMore is not retryable
        // so we want to minimize the number of test that need to be disabled from kill/stepdown
        // suites.
        let res = normalClusterRunCommand.apply(normalCluster,
                                                makeRunCommandArgs({
                                                    find: coll,
                                                    sort: {_id: 1},
                                                    lsid: normalSessionId,
                                                    batchSize: 18446744073709551614
                                                },
                                                                   dbName));
        let cursor0 = new DBCommandCursor(normalCluster.getDB(dbName), res);

        res = normalClusterRunCommand.apply(
            bulkWriteCluster,
            makeRunCommandArgs(
                {find: coll, sort: {_id: 1}, lsid: bulkSessionId, batchSize: 18446744073709551614},
                dbName));
        let cursor1 = new DBCommandCursor(bulkWriteCluster.getDB(dbName), res);

        let diff = null;

        try {
            diff = DataConsistencyChecker.getDiff(cursor0, cursor1);
        } catch (e) {
            jsTestLog("DataConsistencyChecker error during bulkWrite validation");
            jsTestLog(e);

            // Data consistency checker failed, this is because the getMore failed due to stepdown
            // timing. Ignore the error Since this is a forEach block need to return instead of
            // continue, the loop will continue executing after this.
            if (ErrorCodes.isCursorInvalidatedError(e.code)) {
                return;
            }
            throw e;
        }

        // If a CRUD command has been run without an `_id` then each cluster will generate their
        // own _id and the above assert will have failed. To get around this we remove the _id
        // from any document deemed to be "different" and see if the contents of docsMissingOnFirst
        // and docsMissingOnSecond are the same afterwards.
        if (diff.docsWithDifferentContents.length == 0 &&
            diff.docsMissingOnFirst.length == diff.docsMissingOnSecond.length &&
            diff.docsMissingOnFirst.length != 0) {
            for (let i in diff.docsMissingOnFirst) {
                delete diff.docsMissingOnFirst[i]._id;
                delete diff.docsMissingOnSecond[i]._id;
            }
            assert.sameMembers(
                diff.docsMissingOnFirst,
                diff.docsMissingOnSecond,
                `crud_ops_as_bulkWrite: The two clusters have different contents for namespace ${
                    nsInfo.ns} after removing _id`);
        } else {
            assert.eq(
                diff,
                {
                    docsWithDifferentContents: [],
                    docsMissingOnFirst: [],
                    docsMissingOnSecond: [],
                },
                `crud_ops_as_bulkWrite: The two clusters have different contents for namespace ${
                    nsInfo.ns}`);
        }
    });
    Mongo.prototype.runCommand = newRunCommand;
}

function flushBatch(originalRunCommand, makeRunCommandArgs) {
    if (BulkWriteUtils.getCurrentBatchSize() === 0) {
        return;
    }
    try {
        BulkWriteUtils.flushCurrentBulkWriteBatch(bulkWriteCluster,
                                                  bulkSessionId,
                                                  originalRunCommand,
                                                  makeRunCommandArgs,
                                                  true /* isMultiOp */,
                                                  {"errorsOnly": errorsOnly});

        validateClusterConsistency(originalRunCommand, makeRunCommandArgs);
        BulkWriteUtils.resetBulkWriteBatch();
    } catch (error) {
        // In case of error reset the batch.
        BulkWriteUtils.resetBulkWriteBatch();
        jsTestLog("Resetting bulkWrite batch after error");
        jsTestLog(error);
        throw error;
    }
}

function deepCopy(target, source) {
    if (!(target instanceof Object)) {
        return (source === undefined || source === null) ? target : source;
    }

    if (!(source instanceof Object)) {
        return target;
    }

    let res = {};
    Object.keys(source).forEach(k => {
        res[k] = deepCopy(target[k], source[k]);
    });

    return res;
}

function runCommandMultiOpBulkWriteOverride(
    conn, dbName, cmdName, cmdObj, originalRunCommand, makeRunCommandArgs) {
    let cmdCopy = {};
    cmdCopy = deepCopy(cmdCopy, cmdObj);

    // Remove $clusterTime and readConcern from original request since the timestamp might not match
    // between the original and bulkWrite clusters and cause the test to hang.
    delete cmdCopy.$clusterTime;
    delete cmdCopy.readConcern;

    let cmdNameLower = cmdName.toLowerCase();
    if (BulkWriteUtils.canProcessAsBulkWrite(cmdNameLower)) {
        if (!opCompatibleWithCurrentBatch(dbName, cmdCopy[cmdName], cmdCopy)) {
            flushBatch(originalRunCommand, makeRunCommandArgs);
        }

        BulkWriteUtils.processCRUDOp(dbName, cmdNameLower, cmdCopy);
        return normalClusterRunCommand.apply(normalCluster, makeRunCommandArgs(cmdObj));
    }

    // When we encounter a non-CRUD command, first flush the buffered operations on the bulk write
    // cluster. Then, execute the current non-CRUD command against both the bulkWrite and normal
    // clusters.
    flushBatch(originalRunCommand, makeRunCommandArgs);

    cmdCopy.lsid = bulkSessionId;

    // Execute the command against the bulkWrite cluster.
    let res = originalRunCommand.apply(bulkWriteCluster, makeRunCommandArgs(cmdCopy, dbName));

    // Cannot run enableSharding against normal cluster since we always set it up as a replica set.
    if (cmdNameLower == "enablesharding") {
        return res;
    }

    return normalClusterRunCommand.apply(normalCluster, makeRunCommandArgs(cmdObj, dbName));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/multiple_crud_ops_as_bulk_write.js");
OverrideHelpers.overrideRunCommand(runCommandMultiOpBulkWriteOverride);
