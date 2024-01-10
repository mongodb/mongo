/**
 * Overrides the runCommand method to convert specified CRUD ops into bulkWrite commands.
 * Converts the bulkWrite responses into the original CRUD response.
 * This override takes single CRUD ops and sends a bulkWrite and does not batch multiple CRUD ops.
 */
import {BulkWriteUtils} from "jstests/libs/crud_ops_to_bulk_write_lib.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

const errorsOnly = Math.random() < 0.5;

jsTestLog("Running single op bulkWrite override with `errorsOnly:" + errorsOnly + "`");

function getAdditionalParameters(cmdObj) {
    // Deep copy of original command to modify.
    let cmdCopy = {};
    Object.assign(cmdCopy, cmdObj);

    // Remove all parameters we extract for use in bulkWrite.
    ["bypassDocumentValidation",
     "ordered",
     "writeConcern",
     "insert",
     "update",
     "delete",
     "let",
     "sampleId",
     "documents",
     "updates",
     "deletes",
     "collectionUUID",
     "encryptionInformation",
     "isTimeseriesNamespace"]
        .forEach(property => {
            if (cmdCopy.hasOwnProperty(property)) {
                delete cmdCopy[property];
            }
        });
    return cmdCopy;
}

function runCommandSingleOpBulkWriteOverride(
    conn, dbName, cmdName, cmdObj, originalRunCommand, makeRunCommandArgs) {
    let cmdNameLower = cmdName.toLowerCase();
    if (BulkWriteUtils.canProcessAsBulkWrite(cmdNameLower)) {
        BulkWriteUtils.processCRUDOp(dbName, cmdNameLower, cmdObj);
        let additionalParameters = getAdditionalParameters(cmdObj);
        try {
            let response = BulkWriteUtils.flushCurrentBulkWriteBatch(
                conn,
                null /* lsid */,
                originalRunCommand,
                makeRunCommandArgs,
                false /* isMultiOp */,
                {...{"errorsOnly": errorsOnly}, ...additionalParameters});
            assert.eq(response.length, 1);
            BulkWriteUtils.resetBulkWriteBatch();
            return response[0];
        } catch (error) {
            // In case of error reset the batch.
            BulkWriteUtils.resetBulkWriteBatch();
            throw error;
        }
    }

    if (cmdNameLower == "explain") {
        if (BulkWriteUtils.canProcessAsBulkWrite(Object.keys(cmdObj[cmdNameLower])[0])) {
            let letVar = null;
            if (cmdObj[cmdNameLower]["let"]) {
                letVar = cmdObj[cmdNameLower]["let"];
            }
            let key = Object.keys(cmdObj[cmdNameLower])[0];
            BulkWriteUtils.processCRUDOp(dbName, key, cmdObj[cmdNameLower]);
            cmdObj[cmdNameLower] = BulkWriteUtils.getBulkWriteCmd();
            if (letVar) {
                cmdObj[cmdNameLower]["let"] = letVar;
            }
            BulkWriteUtils.resetBulkWriteBatch();

            jsTestLog("New explain: " + tojson(cmdObj));
        }
    }

    // Non-CRUD op, run command as normal and return results.
    return originalRunCommand.apply(conn, makeRunCommandArgs(cmdObj));
}

TestData.runningWithBulkWriteOverride = true;  // See update_metrics.js.

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/single_crud_op_as_bulk_write.js");
OverrideHelpers.overrideRunCommand(runCommandSingleOpBulkWriteOverride);
