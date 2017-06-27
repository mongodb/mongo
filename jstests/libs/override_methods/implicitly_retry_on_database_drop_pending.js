/**
 * Loading this file overrides functions on Mongo.prototype so that operations which return a
 * "DatabaseDropPending" error response are automatically retried until they succeed.
 */
(function() {
    "use strict";

    const defaultTimeout = 10 * 60 * 1000;

    const mongoFindOriginal = Mongo.prototype.find;
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

    Mongo.prototype.find = function find(ns, query, fields, limit, skip, batchSize, options) {
        if (typeof ns !== "string" || !ns.endsWith(".$cmd") || typeof query !== "object" ||
            query === null) {
            return mongoFindOriginal.apply(this, arguments);
        }

        const conn = this;
        const findArguments = arguments;

        const dbName = ns.split(".", 1)[0];
        const commandName = Object.keys(query)[0];

        let originalOps;
        if (commandName === "insert") {
            originalOps = query.documents.slice();
        } else if (commandName === "update") {
            originalOps = query.updates.slice();
        }

        let findRes = mongoFindOriginal.apply(this, arguments);
        const findResNextOriginal = findRes.next;

        // Mongo.prototype.find() lazily returns a JavaScript object repsenting a cursor. The
        // message isn't actually sent to the server until findRes.next() is called, so we must
        // override it as well to create a new cursor should we get an "DatabaseDropPending" error
        // response.
        findRes.next = function next() {
            const nextArguments = arguments;
            let nextResPrevious;
            let nextRes;

            assert.soon(
                () => {
                    nextResPrevious = nextRes;
                    nextRes = findResNextOriginal.apply(findRes, nextArguments);

                    const commandName = Object.keys(query)[0];

                    let opsExecuted;
                    const opsToRetry = [];

                    // We merge ths statistics returned by the server about the number of documents
                    // inserted and updated.
                    if (commandName === "insert") {
                        // We make 'query.documents' refer to 'opsToRetry' to consolidate the logic
                        // for how we retry insert and update operations.
                        opsExecuted = query.documents;
                        query.documents = opsToRetry;

                        if (nextResPrevious !== undefined) {
                            nextRes.n += nextResPrevious.n;
                        }
                    } else if (commandName === "update") {
                        // We make 'query.updates' refer to 'opsToRetry' to consolidate the logic
                        // for how we retry insert and update operations.
                        opsExecuted = query.updates;
                        query.updates = opsToRetry;

                        // The 'upserted' property isn't defined in the response if there weren't
                        // any documents upserted, but we define it as an empty array for
                        // convenience when merging results from 'nextResPrevious'.
                        nextRes.upserted = nextRes.upserted || [];

                        if (nextResPrevious !== undefined) {
                            nextRes.n += nextResPrevious.n;
                            nextRes.nModified += nextResPrevious.nModified;

                            // We translate the 'upsertInfo.index' back to its index in the original
                            // operation that were sent to the server by finding the object's
                            // reference (i.e. using strict-equality) in 'originalOps'.
                            for (let upsertInfo of nextRes.upserted) {
                                upsertInfo.index =
                                    originalOps.indexOf(opsToRetry[upsertInfo.index]);
                            }

                            nextRes.upserted.push(...nextResPrevious.upserted);
                        }
                    }

                    // We anticipate Mongo.prototype.find() is only being used by the bulk API for
                    // performing insert, update, and delete operations, with all other commands
                    // going through Mongo.prototype.runCommand(). We therefore only handle the
                    // "DatabaseDropPending" error response being returned as a write error.
                    if (nextRes.ok !== 1 || !nextRes.hasOwnProperty("writeErrors")) {
                        // If the operation succeeded or failed for another reason, then we simply
                        // return and let the caller deal with the response.
                        return true;
                    }

                    for (let writeError of nextRes.writeErrors) {
                        if (writeError.code !== ErrorCodes.DatabaseDropPending) {
                            // If the operation failed for a reason other than a
                            // "DatabaseDropPending" error response, then we simply return and let
                            // the caller deal with the response.
                            return true;
                        }
                    }

                    // Additional, we only expect the "DatabaseDropPending" error response to be
                    // returned for insert or update operations since deletes shouldn't implicitly
                    // create a collection.
                    if (commandName !== "insert" && commandName !== "update") {
                        throw new Error(
                            "Expected DatabaseDropPending error response to only be returned for" +
                            " write operations that may create a collection, but received it as" +
                            " an error response for '" + commandName + "' command: " +
                            tojson(nextRes));
                    }

                    // We filter out operations that didn't produce a write error to avoid causing a
                    // duplicate key error when retrying the operations.
                    for (let writeError of nextRes.writeErrors) {
                        assert.lt(writeError.index,
                                  opsExecuted.length,
                                  "A write error was returned for an operation outside the list" +
                                      " of operations executed: " + tojson(nextRes));
                        opsToRetry.push(opsExecuted[writeError.index]);
                    }

                    print(
                        commandName + " command failed due to the " + dbName +
                        " database being marked as drop-pending. Waiting for the latest operation" +
                        " to become majority confirmed before trying again.");

                    // We wait for the primary's latest operation to become majority confirmed.
                    // However, we may still need to retry more than once because the primary may
                    // not yet have generated the oplog entry for the "dropDatabase" operation while
                    // it is dropping each intermediate collection.
                    awaitLatestOperationMajorityConfirmed(conn);
                    findRes = mongoFindOriginal.apply(conn, findArguments);
                },
                "timed out while retrying '" + commandName +
                    "' operation on DatabaseDropPending error response for '" + dbName +
                    "' database",
                defaultTimeout);

            return nextRes;
        };

        return findRes;
    };

    Mongo.prototype.runCommand = function(dbName, commandObj, options) {
        if (typeof commandObj !== "object" || commandObj === null) {
            return mongoRunCommandOriginal.apply(this, arguments);
        }

        const conn = this;
        const runCommandArguments = arguments;

        const commandName = Object.keys(commandObj)[0];

        let res;

        assert.soon(
            () => {
                res = mongoRunCommandOriginal.apply(conn, runCommandArguments);

                if (res.ok === 1 || res.code !== ErrorCodes.DatabaseDropPending) {
                    return true;
                }

                print(commandName + " command " + tojsononeline(commandObj) +
                      " failed due to the " + dbName +
                      " database being marked as drop-pending. Waiting for the latest operation" +
                      " to become majority confirmed before trying again.");

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
    };

    Mongo.prototype.runCommandWithMetadata = function(dbName, commandName, metadata, commandArgs) {
        if (typeof commandArgs !== "object" || commandArgs === null) {
            return mongoRunCommandWithMetadataOriginal.apply(this, arguments);
        }

        const conn = this;
        const runCommandWithMetadataArguments = arguments;

        let res;

        assert.soon(
            () => {
                res = mongoRunCommandWithMetadataOriginal.apply(conn,
                                                                runCommandWithMetadataArguments);

                if (res.ok === 1 || res.code !== ErrorCodes.DatabaseDropPending) {
                    return true;
                }

                print(commandName + " command " + tojsononeline(commandArgs) +
                      " failed due to the " + dbName +
                      " database being marked as drop-pending. Waiting for the latest operation" +
                      " to become majority confirmed before trying again.");

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
    };
})();
