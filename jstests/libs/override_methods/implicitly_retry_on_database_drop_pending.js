/**
 * Loading this file overrides functions on Mongo.prototype so that operations which return a
 * "DatabaseDropPending" error response are automatically retried until they succeed.
 */
(function() {
    "use strict";

    const defaultTimeout = 10 * 60 * 1000;

    const mongoRunCommandOriginal = Mongo.prototype.runCommand;
    const mongoRunCommandWithMetadataOriginal = Mongo.prototype.runCommandWithMetadata;

    function awaitLatestOperationMajorityConfirmed(primary) {
        // Get the latest optime from the primary.
        const replSetStatus = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}),
                                                   "error getting replication status from primary");
        const primaryInfo = replSetStatus.members.find(memberInfo => memberInfo.self);
        assert(primaryInfo !== undefined,
               "failed to find self in replication status: " + tojson(replSetStatus));

        // Wait for all operations until 'primaryInfo.optime' to be applied by a majority of the
        // replica set.
        assert.commandWorked(  //
            primary.adminCommand({
                getLastError: 1,
                w: "majority",
                wtimeout: defaultTimeout,
                wOpTime: primaryInfo.optime,
            }),
            "error awaiting replication");
    }

    function runCommandWithRetries(conn, dbName, commandObj, func, funcArgs) {
        if (typeof commandObj !== "object" || commandObj === null) {
            return func.apply(conn, funcArgs);
        }

        const commandName = Object.keys(commandObj)[0];
        let resPrevious;
        let res;

        assert.soon(
            () => {
                resPrevious = res;
                res = func.apply(conn, funcArgs);

                if (commandName === "insert" || commandName === "update") {
                    let opsExecuted;
                    const opsToRetry = [];

                    // We merge ths statistics returned by the server about the number of documents
                    // inserted and updated.
                    if (commandName === "insert") {
                        // We make 'commandObj.documents' refer to 'opsToRetry' to consolidate the
                        // logic for how we retry insert and update operations.
                        opsExecuted = commandObj.documents;
                        commandObj.documents = opsToRetry;

                        if (resPrevious !== undefined) {
                            res.n += resPrevious.n;
                        }
                    } else if (commandName === "update") {
                        // We make 'commandObj.updates' refer to 'opsToRetry' to consolidate the
                        // logic for how we retry insert and update operations.
                        opsExecuted = commandObj.updates;
                        commandObj.updates = opsToRetry;

                        // The 'upserted' property isn't defined in the response if there weren't
                        // any documents upserted, but we define it as an empty array for
                        // convenience when merging results from 'resPrevious'.
                        res.upserted = res.upserted || [];

                        if (resPrevious !== undefined) {
                            res.n += resPrevious.n;
                            res.nModified += resPrevious.nModified;

                            // We translate the 'upsertInfo.index' back to its index in the original
                            // operation that were sent to the server by finding the object's
                            // reference (i.e. using strict-equality) in 'originalOps'.
                            for (let upsertInfo of res.upserted) {
                                upsertInfo.index =
                                    originalOps.indexOf(opsToRetry[upsertInfo.index]);
                            }

                            res.upserted.push(...resPrevious.upserted);
                        }
                    }

                    if (res.ok !== 1 || !res.hasOwnProperty("writeErrors")) {
                        // If the operation succeeded or failed for another reason, then we simply
                        // return and let the caller deal with the response.
                        return true;
                    }

                    for (let writeError of res.writeErrors) {
                        if (writeError.code !== ErrorCodes.DatabaseDropPending) {
                            // If the operation failed for a reason other than a
                            // "DatabaseDropPending" error response, then we simply return and let
                            // the caller deal with the response.
                            return true;
                        }
                    }

                    // We filter out operations that didn't produce a write error to avoid causing a
                    // duplicate key error when retrying the operations. We cache the error message
                    // for the assertion below to avoid the expense of serializing the server's
                    // response as a JSON string repeatedly. (There may be up to 1000 write errors
                    // in the server's response.)
                    const errorMsg =
                        "A write error was returned for an operation outside the list of" +
                        " operations executed: " + tojson(res);

                    for (let writeError of res.writeErrors) {
                        assert.lt(writeError.index, opsExecuted.length, errorMsg);
                        opsToRetry.push(opsExecuted[writeError.index]);
                    }
                } else if (res.ok === 1 || res.code !== ErrorCodes.DatabaseDropPending) {
                    return true;
                }

                let msg = commandName + " command";
                if (commandName !== "insert" && commandName !== "update") {
                    // We intentionally omit the command object in the diagnostic message for
                    // "insert" and "update" commands being retried to avoid printing a large blob
                    // and hurting readability of the logs.
                    msg += " " + tojsononeline(commandObj);
                }

                msg += " failed due to the " + dbName + " database being marked as drop-pending." +
                    " Waiting for the latest operation to become majority confirmed before trying" +
                    " again.";
                print(msg);

                // We wait for the primary's latest operation to become majority confirmed.
                // However, we may still need to retry more than once because the primary may not
                // yet have generated the oplog entry for the "dropDatabase" operation while it is
                // dropping each intermediate collection.
                awaitLatestOperationMajorityConfirmed(conn);
            },
            "timed out while retrying '" + commandName +
                "' operation on DatabaseDropPending error response for '" + dbName + "' database",
            defaultTimeout);

        return res;
    }

    Mongo.prototype.runCommand = function(dbName, commandObj, options) {
        return runCommandWithRetries(this, dbName, commandObj, mongoRunCommandOriginal, arguments);
    };

    Mongo.prototype.runCommandWithMetadata = function(dbName, metadata, commandArgs) {
        return runCommandWithRetries(
            this, dbName, commandArgs, mongoRunCommandWithMetadataOriginal, arguments);
    };
})();
