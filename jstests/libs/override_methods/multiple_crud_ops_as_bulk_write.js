/**
 * Overrides the runCommand method to convert specified CRUD ops into bulkWrite commands.
 * Converts the bulkWrite responses into the original CRUD response.
 */
import {BulkWriteUtils} from "jstests/libs/crud_ops_to_bulk_write_lib.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

let normalCluster = connect(TestData.normalCluster).getMongo();
let bulkWriteCluster = connect(TestData.bulkWriteCluster).getMongo();

jsTestLog("Normal Cluster: " + normalCluster);
jsTestLog("BulkWrite Cluster: " + bulkWriteCluster);

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

function opCompatibleWithCurrentBatch(cmdObj) {
    if (BulkWriteUtils.getCurrentBatchSize() >= maxBatchSize) {
        return false;
    }

    let state = BulkWriteUtils.getBulkWriteState();

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

    return true;
}

function validateClusterConsistency(originalRunCommand, makeRunCommandArgs) {
    // Want to check that every namespace we just altered is the same on both clusters.
    BulkWriteUtils.getNamespaces().forEach(nsInfo => {
        let [dbName, ...coll] = nsInfo.ns.split('.');
        coll = coll.join('.');

        // Using originalRunCommand directly to avoid recursing back into this override file.
        let res = originalRunCommand.apply(normalCluster,
                                           makeRunCommandArgs({find: coll, sort: {_id: 1}}));
        let cursor0 = new DBCommandCursor(normalCluster.getDB(dbName), res);

        res = originalRunCommand.apply(bulkWriteCluster,
                                       makeRunCommandArgs({find: coll, sort: {_id: 1}}));
        let cursor1 = new DBCommandCursor(bulkWriteCluster.getDB(dbName), res);

        const diff = DataConsistencyChecker.getDiff(cursor0, cursor1);

        assert.eq(diff,
                  {
                      docsWithDifferentContents: [],
                      docsMissingOnFirst: [],
                      docsMissingOnSecond: [],
                  },
                  `crud_ops_as_bulkWrite: The two clusters have different contents for namespace ${
                      nsInfo.ns}`);
    });
}

function flushBatch(originalRunCommand, makeRunCommandArgs) {
    if (BulkWriteUtils.getCurrentBatchSize() === 0) {
        return;
    }
    BulkWriteUtils.flushCurrentBulkWriteBatch(
        bulkWriteCluster, originalRunCommand, makeRunCommandArgs);
    validateClusterConsistency(originalRunCommand, makeRunCommandArgs);
    BulkWriteUtils.resetBulkWriteBatch();
}

function runCommandSingleOpBulkWriteOverride(
    conn, dbName, cmdName, cmdObj, originalRunCommand, makeRunCommandArgs) {
    // Run the command always against normalCluster as is and eventually return the results.
    const normalClusterResults =
        originalRunCommand.apply(normalCluster, makeRunCommandArgs(cmdObj));

    let cmdNameLower = cmdName.toLowerCase();
    if (BulkWriteUtils.canProcessAsBulkWrite(cmdNameLower)) {
        if (!opCompatibleWithCurrentBatch(cmdObj)) {
            flushBatch(originalRunCommand, makeRunCommandArgs);
        }

        BulkWriteUtils.processCRUDOp(dbName, cmdNameLower, cmdObj);
        return normalClusterResults;
    }

    // Not a CRUD op that can be converted into bulkWrite, check if we need to flush the current
    // bulkWrite before executing the command.
    if (BulkWriteUtils.commandToFlushBulkWrite(cmdNameLower)) {
        flushBatch(originalRunCommand, makeRunCommandArgs);
    }

    // Execute the command unmodified against the bulkWrite cluster.
    originalRunCommand.apply(bulkWriteCluster, makeRunCommandArgs(cmdObj, "admin"));

    return normalClusterResults;
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/multiple_crud_ops_as_bulk_write.js");
OverrideHelpers.overrideRunCommand(runCommandSingleOpBulkWriteOverride);
