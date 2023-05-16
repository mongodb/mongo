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

function resetBulkWriteBatch() {
    numOpsPerResponse = [];
    nsInfos = [];
    bufferedOps = [];
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

function convertResponse(cmd, orig) {
    let responses = [];
    if (orig.ok == 1) {
        let cursorIdx = 0;
        for (let numOps of numOpsPerResponse) {
            let num = 0;
            let resp = initializeResponse(cmd.ops[cursorIdx]);
            while (num < numOps) {
                if (cursorIdx >= orig.cursor.firstBatch.length) {
                    // this can happen if the bulkWrite encountered an error processing
                    // an op with ordered:true set. This means we have no more op responses
                    // left to process so push the current response we were building and
                    // return.
                    responses.push(resp);
                    return responses;
                }

                let current = orig.cursor.firstBatch[cursorIdx];

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
                        let writeError = {
                            index: current.idx,
                            code: current.code,
                            errmsg: current.errmsg
                        };
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
        "multi": update.multi,
        "upsert": update.upsert
    };

    ["arrayFilters", "collation", "hint", "sampleId"].forEach(property => {
        if (cmdObj.hasOwnProperty(property)) {
            op[property] = cmdObj[property];
        }
    });

    return op;
}

function processDeleteOp(nsInfoIdx, cmdObj, deleteCmd) {
    let op = {"delete": nsInfoIdx, "filter": deleteCmd.q, "multi": deleteCmd.limit == 0};

    ["collation", "hint", "sampleId"].forEach(property => {
        if (cmdObj.hasOwnProperty(property)) {
            op[property] = cmdObj[property];
        }
    });

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

    return op;
}

Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    let cmdName = Object.keys(cmdObj)[0].toLowerCase();
    if (commandsToBulkWriteOverride.has(cmdName)) {
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

        let bulkWriteCmd = {
            "bulkWrite": 1,
            "ops": bufferedOps,
            "nsInfo": nsInfos,
            "ordered": cmdObj.hasOwnProperty("ordered") ? cmdObj.ordered : true
        };
        let res = originalRunCommand.apply(this, ["admin", bulkWriteCmd, options]);
        let finalResponses = convertResponse(bulkWriteCmd, res);
        resetBulkWriteBatch();
        if (finalResponses.length == 1) {
            return finalResponses[0];
        }
        return finalResponses;
    }

    // Not a bulkWrite supported CRUD op, execute the command unmodified.
    return originalRunCommand.apply(this, arguments);
};
})();
