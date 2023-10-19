/**
 * Overrides the runCommand method to convert specified CRUD ops into bulkWrite commands.
 * Converts the bulkWrite responses into the original CRUD response.
 * This override takes single CRUD ops and sends a bulkWrite and does not batch multiple CRUD ops.
 */
import {BulkWriteUtils} from "jstests/libs/crud_ops_to_bulk_write_lib.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

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
            let response = BulkWriteUtils.flushCurrentBulkWriteBatch(conn,
                                                                     null /* lsid */,
                                                                     originalRunCommand,
                                                                     makeRunCommandArgs,
                                                                     additionalParameters);
            assert.eq(response.length, 1);
            BulkWriteUtils.resetBulkWriteBatch();
            return response[0];
        } catch (error) {
            // In case of error reset the batch.
            BulkWriteUtils.resetBulkWriteBatch();
            throw error;
        }
    }

    // Non-CRUD op, run command as normal and return results.
    return originalRunCommand.apply(conn, makeRunCommandArgs(cmdObj));
}

TestData.runningWithBulkWriteOverride = true;  // See update_metrics.js.

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/single_crud_op_as_bulk_write.js");
OverrideHelpers.overrideRunCommand(runCommandSingleOpBulkWriteOverride);
