/**
 * Overrides the runCommand method to convert specified CRUD ops into bulkWrite commands.
 * Converts the bulkWrite responses into the original CRUD response.
 */
(function() {
'use strict';

let originalRunCommand = Mongo.prototype.runCommand;

let normalCluster = connect(TestData.normalCluster).getMongo();
let bulkWriteCluster = connect(TestData.bulkWriteCluster).getMongo();

jsTestLog("Normal Cluster: " + normalCluster);
jsTestLog("BulkWrite Cluster: " + bulkWriteCluster);

const commandsToBulkWriteOverride = new Set(["insert", "update", "delete"]);

const commandsToAlwaysFlushBulkWrite = new Set([
    "aggregate",
    "mapreduce",
    "authenticate",
    "logout",
    "applyops",
    "checkshardingindex",
    "cleanuporphaned",
    "cleanupreshardcollection",
    "commitreshardcollection",
    "movechunk",
    "moveprimary",
    "moverange",
    "mergechunks",
    "refinecollectionshardkey",
    "split",
    "splitvector",
    "killallsessions",
    "killallsessionsbypattern",
    "dropconnections",
    "filemd5",
    "fsync",
    "fsyncunlock",
    "killop",
    "setfeaturecompatibilityversion",
    "shutdown",
    "currentop",
    "listdatabases",
    "listcollections",
    "committransaction",
    "aborttransaction",
    "preparetransaction",
    "endsessions",
    "killsessions"
]);

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

function validateClusterConsistency(options) {
    // Want to check that every namespace we just altered is the same on both clusters.
    nsInfos.forEach(nsInfo => {
        let [dbName, ...coll] = nsInfo.ns.split('.');
        coll = coll.join('.');

        // Using originalRunCommand directly to avoid recursing back into this override file.
        let res = originalRunCommand.apply(normalCluster,
                                           [dbName, {find: coll, sort: {_id: 1}}, options]);
        let cursor0 = new DBCommandCursor(normalCluster.getDB(dbName), res);

        res = originalRunCommand.apply(bulkWriteCluster,
                                       [dbName, {find: coll, sort: {_id: 1}}, options]);
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
    resp = originalRunCommand.apply(bulkWriteCluster, ["admin", bulkWriteCmd, options]);

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

        resp = originalRunCommand.apply(bulkWriteCluster, ["admin", bulkWriteCmd, options]);
        response = convertBulkWriteResponse(bulkWriteCmd, resp);
        finalResponse = finalResponse.concat(response);
    }

    // After performing the bulkWrite make sure the two clusters have the same documents on
    // impacted collections.
    validateClusterConsistency(options);

    resetBulkWriteBatch();
    return response;
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

Mongo.prototype.runCommand = function(dbName, cmdObj, options) {
    // Run the command always against normalCluster as is and eventually return the results.
    const normalClusterResults = originalRunCommand.apply(normalCluster, arguments);

    let cmdName = Object.keys(cmdObj)[0].toLowerCase();
    if (commandsToBulkWriteOverride.has(cmdName)) {
        if (!opCompatibleWithCurrentBatch(cmdObj)) {
            flushCurrentBulkWriteBatch.apply(bulkWriteCluster, [options]);
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
        } else {
            throw new Error("Unrecognized command in bulkWrite override");
        }

        numOpsPerResponse.push(numOps);

        return normalClusterResults;
    }

    // Not a CRUD op that can be converted into bulkWrite, check if we need to flush the current
    // bulkWrite before executing the command.
    if (commandsToAlwaysFlushBulkWrite.has(cmdName)) {
        flushCurrentBulkWriteBatch.apply(bulkWriteCluster, [options]);
    }

    // Execute the command unmodified against the bulkWrite cluster.
    originalRunCommand.apply(bulkWriteCluster, arguments);

    return normalClusterResults;
};
})();
