/**
 * Utility functions used to convert CRUD ops into a bulkWrite command.
 * Converts the bulkWrite responses into the original CRUD response.
 */
export const BulkWriteUtils = (function() {
    const commandsToBulkWriteOverride = new Set(["insert", "update", "delete"]);

    let numOpsPerResponse = [];
    let nsInfos = [];
    let bufferedOps = [];
    let letObj = null;
    let wc = null;
    let ordered = true;
    let bypassDocumentValidation = null;
    let hasUpsert = false;

    function canProcessAsBulkWrite(cmdName) {
        return commandsToBulkWriteOverride.has(cmdName);
    }

    function resetBulkWriteBatch() {
        numOpsPerResponse = [];
        nsInfos = [];
        bufferedOps = [];
        letObj = null;
        wc = null;
        bypassDocumentValidation = null;
        ordered = true;
        hasUpsert = false;
    }

    function getCurrentBatchSize() {
        return numOpsPerResponse.length;
    }

    function getBulkWriteState() {
        return {
            nsInfos: nsInfos,
            bypassDocumentValidation: bypassDocumentValidation,
            letObj: letObj,
            ordered: ordered,
        };
    }

    function getNamespaces() {
        return nsInfos;
    }

    function getBulkWriteCmd() {
        return {
            "bulkWrite": 1,
            "ops": bufferedOps,
            "nsInfo": nsInfos,
            "ordered": (ordered != null) ? ordered : true,
            "bypassDocumentValidation": bypassDocumentValidation,
        };
    }

    function flushCurrentBulkWriteBatch(
        conn, lsid, originalRunCommand, makeRunCommandArgs, isMultiOp, additionalParameters = {}) {
        // Should not be possible to reach if bypassDocumentValidation is not set.
        assert(bypassDocumentValidation != null);

        let bulkWriteCmd = {
            "bulkWrite": 1,
            "ops": bufferedOps,
            "nsInfo": nsInfos,
            "ordered": (ordered != null) ? ordered : true,
            "bypassDocumentValidation": bypassDocumentValidation,
        };

        if (wc != null) {
            bulkWriteCmd["writeConcern"] = wc;
        }

        if (letObj != null) {
            bulkWriteCmd["let"] = letObj;
        }

        if (lsid) {
            bulkWriteCmd["lsid"] = lsid;
        }

        // Add in additional parameters to the bulkWrite command.
        bulkWriteCmd = {...bulkWriteCmd, ...additionalParameters};

        if (!isMultiOp && !canBeErrorsOnlySingleCrudOp()) {
            bulkWriteCmd["errorsOnly"] = false;
        }

        let resp = originalRunCommand.apply(conn, makeRunCommandArgs(bulkWriteCmd, "admin"));

        let response = convertBulkWriteResponse(bulkWriteCmd, isMultiOp, resp);
        let finalResponse = response;

        let expectedResponseLength = numOpsPerResponse.length;

        // The following blocks are only relevant for batching multiple commands together
        // to ensure that every separate command is run even if previous commands fail.
        // The way we do this is by removing all ops that were executed from `ops`, and also
        // remove any ops that came from the same command. For example if we made a batch of
        // bulkWrite = insert1 + insert2 where insert1 = [{a:1},{b:1},{c:1}], and insert2 = [{d:1}]
        // and the op at index 1 failed we would re-run the bulkWrite with just `{d:1}` since we
        // need to achive the result of running both insert1 + insert2.
        if (bulkWriteCmd.errorsOnly == true) {
            // For errorsOnly we will only ever have items in the response cursor if an operation
            // failed. We also always run batched bulkWrites as ordered:true so only one command can
            // fail. Once we get a bulkWrite with no errors then we have executed all ops.
            while ((resp.cursor == null) || (resp.cursor.firstBatch.length != 0)) {
                let idx = resp.cursor ? resp.cursor.firstBatch[0].idx : 0;
                let i = 0;
                while (i <= idx) {
                    let numOpsToShift = numOpsPerResponse.shift();
                    for (let j = 0; j < numOpsToShift; j++) {
                        bufferedOps.shift();
                        i++;
                    }
                    // If there were no ops we need to progress forward to avoid infinite looping.
                    if (numOpsToShift == 0) {
                        i++;
                    }
                }

                // Can't execute a bulkWrite with no ops remaining.
                if (bufferedOps.length == 0) {
                    break;
                }

                bulkWriteCmd.ops = bufferedOps;
                resp = originalRunCommand.apply(conn, makeRunCommandArgs(bulkWriteCmd, "admin"));
                response = convertBulkWriteResponse(bulkWriteCmd, isMultiOp, resp);
                finalResponse = finalResponse.concat(response);
            }
        } else {
            // Retry on ordered:true failures by re-running subset of original bulkWrite command.
            while (finalResponse.length != expectedResponseLength) {
                // Need to figure out how many ops we need to subset out. Every entry in
                // numOpsPerResponse represents a number of bulkWrite ops that correspond to an
                // initial CRUD op. We need to make sure we split at a CRUD op boundary in the
                // bulkWrite.
                for (let i = 0; i < response.length; i++) {
                    let target = numOpsPerResponse.shift();
                    for (let j = 0; j < target; j++) {
                        bufferedOps.shift();
                    }
                }
                bulkWriteCmd.ops = bufferedOps;
                resp = originalRunCommand.apply(conn, makeRunCommandArgs(bulkWriteCmd, "admin"));
                response = convertBulkWriteResponse(bulkWriteCmd, isMultiOp, resp);
                finalResponse = finalResponse.concat(response);
            }
        }

        return finalResponse;
    }

    function initializeResponse(op) {
        if (op.hasOwnProperty("update")) {
            // Update always has nModified field set.
            return {"n": 0, "nModified": 0, "ok": 1};
        }
        return {"n": 0, "ok": 1};
    }

    function canBeErrorsOnlySingleCrudOp() {
        // The conditions we need to meet are as follows:
        // An updateOne, deleteOne, insertOne, or insertMany command

        // Multiple crud ops make up this batch.
        if (numOpsPerResponse.length != 1) {
            assert.eq(0, numOpsPerResponse.length);
            return false;
        }

        if (hasUpsert) {
            return false;
        }

        return true;
    }

    function processErrorsOnlySingleCrudOpResponse(cmd, bulkWriteResponse) {
        // Need to construct the original command response based on the summary fields.
        let response = initializeResponse(cmd.ops[0]);

        if (cmd.ops[0].hasOwnProperty("insert")) {
            response.n += bulkWriteResponse.nInserted;
        } else if (cmd.ops[0].hasOwnProperty("update")) {
            response.n += bulkWriteResponse.nMatched;
            response.nModified += bulkWriteResponse.nModified;
        } else if (cmd.ops[0].hasOwnProperty("delete")) {
            response.n += bulkWriteResponse.nDeleted;
        } else {
            throw new Error("Invalid bulkWrite op type. " + cmd.ops[0]);
        }

        ["writeConcernError",
         "retriedStmtIds",
         "opTime",
         "$clusterTime",
         "electionId",
         "operationTime",
         "errorLabels",
         "_mongo"]
            .forEach(property => {
                if (bulkWriteResponse.hasOwnProperty(property)) {
                    response[property] = bulkWriteResponse[property];
                }
            });

        // Need to loop through any errors now.
        if (bulkWriteResponse.cursor.firstBatch.length != 0) {
            let cursorIdx = 0;
            while (cursorIdx < bulkWriteResponse.cursor.firstBatch.length) {
                let current = bulkWriteResponse.cursor.firstBatch[cursorIdx];
                // For errorsOnly every cursor element must be an error.
                assert.eq(0,
                          current.ok,
                          "command: " + tojson(cmd) + " : response: " + tojson(bulkWriteResponse));

                if (!response.hasOwnProperty("writeErrors")) {
                    response["writeErrors"] = [];
                }
                let writeError = {index: current.idx, code: current.code, errmsg: current.errmsg};

                // Include optional error fields if they exist.
                ["errInfo", "db", "collectionUUID", "expectedCollection", "actualCollection"]
                    .forEach(property => {
                        if (current.hasOwnProperty(property)) {
                            writeError[property] = current[property];
                        }
                    });

                response["writeErrors"].push(writeError);
                cursorIdx++;
            }
        }

        return response;
    }

    /**
     * The purpose of this function is to take a server response from a bulkWrite command and to
     * transform it to an array of responses for the corresponding CRUD commands that make up the
     * bulkWrite.
     *
     * 'cmd' is the bulkWrite that was executed to generate the response
     * 'orig' is the bulkWrite command response
     */
    function convertBulkWriteResponse(cmd, isMultiOp, bulkWriteResponse) {
        // a w0 write concern bulkWrite can result in just {ok: 1}, so if a response does not have
        // 'cursor' field then just return the response as is
        if (!bulkWriteResponse.cursor) {
            return [bulkWriteResponse];
        }

        // Handle processing response for single CRUD op with errors only.
        // The multi op code can sometimes take this path but we don't care about the response
        // conversion for that so it is okay.
        if (cmd.errorsOnly == true && !isMultiOp && canBeErrorsOnlySingleCrudOp()) {
            return [processErrorsOnlySingleCrudOpResponse(cmd, bulkWriteResponse)];
        }

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
                        // If the last response has writeErrors set then it was in the middle of an
                        // op otherwise we are beginning a new op response and should not push it.
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

                        // Include optional error fields if they exist.
                        ["errInfo",
                         "db",
                         "collectionUUID",
                         "expectedCollection",
                         "actualCollection"]
                            .forEach(property => {
                                if (current.hasOwnProperty(property)) {
                                    writeError[property] = current[property];
                                }
                            });

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
                            // Need to add the index of the upserted doc.
                            resp["upserted"].push({index: cursorIdx, ...current.upserted});
                        }
                    }

                    ["writeConcernError",
                     "retriedStmtIds",
                     "opTime",
                     "$clusterTime",
                     "electionId",
                     "operationTime",
                     "errorLabels",
                     "_mongo"]
                        .forEach(property => {
                            if (bulkWriteResponse.hasOwnProperty(property)) {
                                resp[property] = bulkWriteResponse[property];
                            }
                        });

                    cursorIdx += 1;
                    num += 1;
                }
                responses.push(resp);
            }
        }
        return responses;
    }

    function getNsInfoIdx(
        nsInfoEntry, collectionUUID, encryptionInformation, isTimeseriesNamespace) {
        let idx = nsInfos.findIndex((element) => element.ns == nsInfoEntry);
        if (idx == -1) {
            idx = nsInfos.length;
            let nsInfo = {ns: nsInfoEntry};
            if (collectionUUID) {
                nsInfo["collectionUUID"] = collectionUUID;
            }
            if (encryptionInformation) {
                nsInfo["encryptionInformation"] = encryptionInformation;
            }
            if (isTimeseriesNamespace) {
                nsInfo["isTimeseriesNamespace"] = isTimeseriesNamespace;
            }
            nsInfos.push(nsInfo);
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
            "upsert": update.upsert ? update.upsert : false
        };

        ["arrayFilters", "collation", "hint", "sampleId", "sort", "upsertSupplied"].forEach(
            property => {
                if (update.hasOwnProperty(property)) {
                    op[property] = update[property];
                }
            });

        if (update.hasOwnProperty("c")) {
            op["constants"] = update.c;
        }

        if (cmdObj.hasOwnProperty("let")) {
            letObj = cmdObj.let;
        }

        if (op.upsert == true) {
            hasUpsert = true;
        }

        return op;
    }

    function processDeleteOp(nsInfoIdx, cmdObj, deleteCmd) {
        let op = {"delete": nsInfoIdx, "filter": deleteCmd.q, "multi": deleteCmd.limit == 0};

        ["sampleId", "collation", "hint"].forEach(property => {
            if (deleteCmd.hasOwnProperty(property)) {
                op[property] = deleteCmd[property];
            }
        });

        if (cmdObj.hasOwnProperty("let")) {
            letObj = cmdObj.let;
        }

        return op;
    }

    function processCRUDOp(dbName, cmdName, cmdObj) {
        // Set bypassDocumentValidation if necessary.
        if (bypassDocumentValidation == null) {
            bypassDocumentValidation = cmdObj.hasOwnProperty("bypassDocumentValidation")
                ? cmdObj.bypassDocumentValidation
                : false;
        }

        ordered = cmdObj.hasOwnProperty("ordered") ? cmdObj.ordered : true;

        if (cmdObj.hasOwnProperty("writeConcern")) {
            wc = cmdObj.writeConcern;
        }

        let nsInfoEntry = dbName + "." + cmdObj[cmdName];
        let nsInfoIdx = getNsInfoIdx(nsInfoEntry,
                                     cmdObj.collectionUUID,
                                     cmdObj.encryptionInformation,
                                     cmdObj.isTimeseriesNamespace);

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
    }

    return {
        processCRUDOp: processCRUDOp,
        getNsInfoIdx: getNsInfoIdx,
        flushCurrentBulkWriteBatch: flushCurrentBulkWriteBatch,
        resetBulkWriteBatch: resetBulkWriteBatch,
        canProcessAsBulkWrite: canProcessAsBulkWrite,
        getCurrentBatchSize: getCurrentBatchSize,
        getBulkWriteState: getBulkWriteState,
        getNamespaces: getNamespaces,
        getBulkWriteCmd: getBulkWriteCmd
    };
})();
