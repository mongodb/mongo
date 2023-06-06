/**
 * Overrides the runCommand method to convert specified CRUD ops into bulkWrite commands.
 * Converts the bulkWrite responses into the original CRUD response.
 */
(function() {
'use strict';

let originalRunCommand = Mongo.prototype.runCommand;

const commandsToBulkWriteOverride = new Set(["insert", "update", "delete", "findandmodify"]);

let numOpsPerResponse = [];
let nsInfos = [];
let bufferedOps = [];
let letObj = null;
let ordered = true;
let bypassDocumentValidation = null;
const maxBatchSize = 5;

function resetBulkWriteBatch() {
    numOpsPerResponse = [];
    nsInfos = [];
    bufferedOps = [];
    letObj = null;
    bypassDocumentValidation = null;
    ordered = true;
}

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
    if (numOpsPerResponse.length >= maxBatchSize) {
        return false;
    }

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

    // If 'letObj' is null then we can always continue. If 'letObj' is not null and cmdObj.let is
    // then we can always continue. If both objects are not null and they are the same we can
    // continue.
    if (letObj != null && currentCmdLet != null && 0 === bsonWoCompare(letObj, currentCmdLet)) {
        return false;
    }

    // If saved ordered is false or the incoming ordered is false we must flush the batch.
    let newOrdered = cmdObj.hasOwnProperty("ordered") ? cmdObj.ordered : true;
    if (!ordered || !newOrdered) {
        return false;
    }

    return true;
}

function flushCurrentBulkWriteBatch(options) {
    if (bufferedOps.length == 0) {
        return {};
    }

    // Should not be possible to reach if bypassDocumentValidation is not set.
    assert(bypassDocumentValidation != null);

    let bulkWriteCmd = {
        "bulkWrite": 1,
        "ops": bufferedOps,
        "nsInfo": nsInfos,
        "ordered": (ordered != null) ? ordered : true,
        "bypassDocumentValidation": bypassDocumentValidation,
    };

    if (letObj != null) {
        bulkWriteCmd["let"] = letObj;
    }

    let resp = {};
    resp = originalRunCommand.apply(this, ["admin", bulkWriteCmd, options]);

    let response = convertBulkWriteResponse(bulkWriteCmd, resp);
    let finalResponse = response;

    let expectedResponseLength = numOpsPerResponse.length;

    // Retry on ordered:true failures by re-running subset of original bulkWrite command.
    while (finalResponse.length != expectedResponseLength) {
        // Need to figure out how many ops we need to subset out. Every entry in numOpsPerResponse
        // represents a number of bulkWrite ops that correspond to an initial CRUD op. We need to
        // make sure we split at a CRUD op boundary in the bulkWrite.
        for (let i = 0; i < response.length; i++) {
            let target = numOpsPerResponse.shift();
            for (let j = 0; j < target; j++) {
                bufferedOps.shift();
            }
        }
        bulkWriteCmd.ops = bufferedOps;

        resp = originalRunCommand.apply(this, ["admin", bulkWriteCmd, options]);
        response = convertBulkWriteResponse(bulkWriteCmd, resp);
        finalResponse = finalResponse.concat(response);
    }

    resetBulkWriteBatch();
    return response;
}

function processFindAndModifyResponse(current, isRemove, resp) {
    // findAndModify will only ever be a single op so we can freely replace
    // the existing response.
    resp = {ok: 1, value: null};
    if (current.hasOwnProperty("value")) {
        resp["value"] = current.value;
    }
    let lastErrorObject = {};
    lastErrorObject["n"] = current.n;
    if (current.hasOwnProperty("upserted")) {
        lastErrorObject["upserted"] = current.upserted._id;
    }
    if (!isRemove) {
        lastErrorObject["updatedExisting"] = current.nModified != 0;
    }
    resp["lastErrorObject"] = lastErrorObject;
    return resp;
}

function initializeResponse(op) {
    if (op.hasOwnProperty("update")) {
        // Update always has nModified field set.
        return {"n": 0, "nModified": 0, "ok": 1};
    }
    return {"n": 0, "ok": 1};
}

/**
 * The purpose of this function is to take a server response from a bulkWrite command and to
 * transform it to an array of responses for the corresponding CRUD commands that make up the
 * bulkWrite.
 *
 * 'cmd' is the bulkWrite that was executed to generate the response
 * 'orig' is the bulkWrite command response
 */
function convertBulkWriteResponse(cmd, bulkWriteResponse) {
    let responses = [];
    if (bulkWriteResponse.ok == 1) {
        let cursorIdx = 0;
        for (let numOps of numOpsPerResponse) {
            let num = 0;
            let resp = initializeResponse(cmd.ops[cursorIdx]);
            while (num < numOps) {
                if (cursorIdx >= bulkWriteResponse.cursor.firstBatch.length) {
                    // this can happen if the bulkWrite encountered an error processing
                    // an op with ordered:true set. This means we have no more op responses
                    // left to process so push the current response we were building and
                    // return.
                    // If the last response has writeErrors set then it was in the middle of an op
                    // otherwise we are beginning a new op response and should not push it.
                    if (resp.writeErrors) {
                        responses.push(resp);
                    }
                    return responses;
                }

                let current = bulkWriteResponse.cursor.firstBatch[cursorIdx];

                // findAndModify returns have a different format. Detect findAndModify
                // by the precense of 'return' field in the op.
                if (cmd.ops[cursorIdx].hasOwnProperty("return")) {
                    resp = processFindAndModifyResponse(
                        current, cmd.ops[cursorIdx].hasOwnProperty("delete"), resp);
                } else {
                    if (current.ok == 0) {
                        // Normal write contains an error.
                        if (!resp.hasOwnProperty("writeErrors")) {
                            resp["writeErrors"] = [];
                        }
                        let writeError = {index: num, code: current.code, errmsg: current.errmsg};
                        resp["writeErrors"].push(writeError);
                    } else {
                        resp.n += current.n;
                        if (current.hasOwnProperty("nModified")) {
                            resp.nModified += current.nModified;
                        }
                        if (current.hasOwnProperty("upserted")) {
                            if (!resp.hasOwnProperty("upserted")) {
                                resp["upserted"] = [];
                            }
                            resp["upserted"].push(current.upserted);
                        }
                    }
                }
                cursorIdx += 1;
                num += 1;
            }
            responses.push(resp);
        }
    }
    return responses;
}

function getNsInfoIdx(nsInfoEntry) {
    let idx = nsInfos.findIndex((element) => element.ns == nsInfoEntry);
    if (idx == -1) {
        idx = nsInfos.length;
        nsInfos.push({ns: nsInfoEntry});
    }
    return idx;
}

function processInsertOp(nsInfoIdx, doc) {
    return {insert: nsInfoIdx, document: doc};
}

function processUpdateOp(nsInfoIdx, cmdObj, update) {
    let op = {
        "update": nsInfoIdx,
        "filter": update.q,
        "updateMods": update.u,
        "multi": update.multi ? update.multi : false,
        "upsert": update.upsert ? update.upsert : false,
    };

    ["arrayFilters", "collation", "hint", "sampleId"].forEach(property => {
        if (cmdObj.hasOwnProperty(property)) {
            op[property] = cmdObj[property];
        }
    });

    if (update.hasOwnProperty("let")) {
        letObj = update.let;
    }

    return op;
}

function processDeleteOp(nsInfoIdx, cmdObj, deleteCmd) {
    let op = {
        "delete": nsInfoIdx,
        "filter": deleteCmd.q,
        "multi": deleteCmd.limit ? deleteCmd.limit == 0 : false
    };

    ["collation", "hint", "sampleId"].forEach(property => {
        if (cmdObj.hasOwnProperty(property)) {
            op[property] = cmdObj[property];
        }
    });

    if (deleteCmd.hasOwnProperty("let")) {
        letObj = deleteCmd.let;
    }

    return op;
}

function processFindAndModifyOp(nsInfoIdx, cmdObj) {
    let op = {};

    if (cmdObj.hasOwnProperty("remove") && (cmdObj.remove == true)) {
        // is delete.
        op["delete"] = nsInfoIdx;
        op["return"] = true;
    } else {
        // is update.
        op["update"] = nsInfoIdx;
        op["updateMods"] = cmdObj.update;
        op["return"] = cmdObj.new ? "post" : "pre";
        if (cmdObj.hasOwnProperty("upsert")) {
            op["upsert"] = cmdObj.upsert;
        }
        if (cmdObj.hasOwnProperty("arrayFilters")) {
            op["arrayFilters"] = cmdObj.arrayFilters;
        }
    }

    op["filter"] = cmdObj.query;

    ["sort", "collation", "hint", "sampleId"].forEach(property => {
        if (cmdObj.hasOwnProperty(property)) {
            op[property] = cmdObj[property];
        }
    });

    if (cmdObj.hasOwnProperty("fields")) {
        op["returnFields"] = cmdObj.fields;
    }

    if (cmdObj.hasOwnProperty("let")) {
        letObj = cmdObj.let;
    }

    return op;
}

Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    /**
     * After SERVER-76660 this function will be used to direct a command to 2 different clusters.
     * The main cluster will always execute originalRunCommand and the second will follow the
     * current execution path below and their responses will be compared (if the bulkWrite path
     * executed anything).
     */

    let cmdName = Object.keys(cmdObj)[0].toLowerCase();
    if (commandsToBulkWriteOverride.has(cmdName)) {
        let response = {};
        if (!opCompatibleWithCurrentBatch(cmdObj)) {
            response = flushCurrentBulkWriteBatch.apply(this, [options]);
        }

        // Set bypassDocumentValidation if necessary.
        if (bypassDocumentValidation == null) {
            bypassDocumentValidation = cmdObj.hasOwnProperty("bypassDocumentValidation")
                ? cmdObj.bypassDocumentValidation
                : false;
        }

        ordered = cmdObj.hasOwnProperty("ordered") ? cmdObj.ordered : true;

        let nsInfoEntry = dbName + "." + cmdObj[cmdName];
        let nsInfoIdx = getNsInfoIdx(nsInfoEntry);

        let numOps = 0;

        // Is insert
        if (cmdName === "insert") {
            assert(cmdObj.documents);
            for (let doc of cmdObj.documents) {
                bufferedOps.push(processInsertOp(nsInfoIdx, doc));
                numOps += 1;
            }
        } else if (cmdName === "update") {
            assert(cmdObj.updates);
            for (let update of cmdObj.updates) {
                bufferedOps.push(processUpdateOp(nsInfoIdx, cmdObj, update));
                numOps += 1;
            }
        } else if (cmdName === "delete") {
            assert(cmdObj.deletes);
            for (let deleteCmd of cmdObj.deletes) {
                bufferedOps.push(processDeleteOp(nsInfoIdx, cmdObj, deleteCmd));
                numOps += 1;
            }
        } else if (cmdName === "findandmodify") {
            bufferedOps.push(processFindAndModifyOp(nsInfoIdx, cmdObj));
            numOps += 1;
        } else {
            throw new Error("Unrecognized command in bulkWrite override");
        }

        numOpsPerResponse.push(numOps);

        return response;
    }

    // Flush current bulkWrite batch on non-CRUD command.
    flushCurrentBulkWriteBatch.apply(this, [options]);

    // Not a bulkWrite supported CRUD op, execute the command unmodified.
    return originalRunCommand.apply(this, arguments);
};
})();
