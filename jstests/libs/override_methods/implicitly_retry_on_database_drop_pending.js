/**
 * Loading this file overrides functions on Mongo.prototype so that operations which return a
 * "DatabaseDropPending" error response are automatically retried until they succeed.
 */
const defaultTimeout = 10 * 60 * 1000;

const mongoRunCommandOriginal = Mongo.prototype.runCommand;

function runCommandWithRetries(conn, dbName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    // We create a copy of 'commandObj' to avoid mutating the parameter the caller specified.
    // Instead, we use the makeFuncArgs() function to build the array of arguments to 'func' by
    // giving it the 'commandObj' that should be used. This is done to work around the
    // difference in the order of parameters for the Mongo.prototype.runCommand() function.
    commandObj = Object.assign({}, commandObj);
    const commandName = Object.keys(commandObj)[0];
    let resPrevious;
    let res;

    assert.soon(
        () => {
            resPrevious = res;
            res = func.apply(conn, makeFuncArgs(commandObj));

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
                            /* eslint-disable-next-line */
                            upsertInfo.index = originalOps.indexOf(opsToRetry[upsertInfo.index]);
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
                    " operations executed: " +
                    tojson(res);

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

            msg += " failed due to the " + dbName + " database being marked as drop-pending.";
            jsTest.log.info(msg);

            if (TestData.skipDropDatabaseOnDatabaseDropPending && commandName === "dropDatabase") {
                // We avoid retrying the "dropDatabase" command when another "dropDatabase"
                // command was already in progress for the database. This reduces the likelihood
                // that other clients would observe another DatabaseDropPending error response
                // when they go to retry, and therefore reduces the risk that repeatedly
                // retrying an individual operation would take longer than the 'defaultTimeout'
                // period.
                res = {ok: 1, dropped: dbName};
                return true;
            }
        },
        "timed out while retrying '" +
            commandName +
            "' operation on DatabaseDropPending error response for '" +
            dbName +
            "' database",
        defaultTimeout,
    );

    return res;
}

Mongo.prototype.runCommand = function (dbName, commandObj, options) {
    return runCommandWithRetries(this, dbName, commandObj, mongoRunCommandOriginal, (commandObj) => [
        dbName,
        commandObj,
        options,
    ]);
};
