/**
 * Overrides runCommand so operations that encounter the BackgroundOperationInProgressForNs/Db error
 * codes automatically retry.
 */

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// Values in msecs.
const kRetryTimeout = 10 * 60 * 1000;
const kIntervalBetweenRetries = 50;

function _runAndExhaustQueryWithRetryUponMigration(
    conn, commandName, commandObj, func, makeFuncArgs) {
    const kQueryRetryableErrors = [
        ErrorCodes.ReshardCollectionInProgress,
        ErrorCodes.ConflictingOperationInProgress,
        ErrorCodes.QueryPlanKilled
    ];

    let queryResponse;
    let attempt = 0;
    const collName = commandObj[commandName];
    const lsid = commandObj['lsid'];
    const apiVersion = commandObj['apiVersion'];
    const apiStrict = commandObj['apiStrict'];

    assert.soon(
        () => {
            attempt++;
            queryResponse = func.apply(conn, makeFuncArgs(commandObj));
            let latestBatchResponse = queryResponse;
            while (latestBatchResponse.ok === 1 && latestBatchResponse.cursor &&
                   latestBatchResponse.cursor.id != 0) {
                // Exhaust the cursor returned by the command and return to the test a single batch
                // containing all the results.
                const getMoreCommandObj = {
                    getMore: latestBatchResponse.cursor.id,
                    collection: collName,
                    lsid: lsid,
                    apiVersion: apiVersion,
                    apiStrict: apiStrict,
                };
                latestBatchResponse = func.apply(conn, makeFuncArgs(getMoreCommandObj));

                if (latestBatchResponse.ok === 1) {
                    queryResponse.cursor.firstBatch.push(...latestBatchResponse.cursor.nextBatch);
                    queryResponse.cursor.id = NumberLong(0);
                } else {
                    jsTest.log(`An error occurred while attempting to exhaust the results of ${
                        commandName} at attempt ${attempt}: ${tojson(latestBatchResponse)}`);
                }
            }

            let stopRetrying = false;
            if (latestBatchResponse.ok === 1) {
                stopRetrying = true;
            } else if (!kQueryRetryableErrors.includes(latestBatchResponse.code)) {
                // Non-retryable error detected; forward the response to the test.
                stopRetrying = true;
                queryResponse = latestBatchResponse;
            }

            return stopRetrying;
        },
        () => "Timed out while retrying command '" + tojson(commandObj) +
            "', response: " + tojson(queryResponse),
        kRetryTimeout,
        kIntervalBetweenRetries);

    return queryResponse;
}

function _runCommandWithRetryUponMigration(conn, commandName, commandObj, func, makeFuncArgs) {
    // These are all commands that can return BackgroundOperationInProgress error codes.
    const kRetryableCommands = new Set([
        "createIndexes",
        "moveCollection",
        "reshardCollection",
        "unshardCollection",
    ]);

    const kCommandRetryableErrors =
        [ErrorCodes.ReshardCollectionInProgress, ErrorCodes.ConflictingOperationInProgress];

    const kNoRetry = true;
    const kRetry = false;

    let commandResponse;
    let attempt = 0;

    assert.soon(
        () => {
            attempt++;

            commandResponse = func.apply(conn, makeFuncArgs(commandObj));
            if (commandResponse.ok === 1) {
                return kNoRetry;
            }

            // Commands that are not in the allowlist should never fail with this error code.
            if (!kRetryableCommands.has(commandName)) {
                return kNoRetry;
            }

            let message = "Retrying the " + commandName +
                " command because a migration operation is in progress (attempt " + attempt +
                "): " + tojson(commandResponse);

            // This handles the retry case when run against a standalone, replica set, or mongos
            // where both shards returned the same response.
            if (kCommandRetryableErrors.includes(commandResponse.code)) {
                jsTestLog(message);
                return kRetry;
            }

            jsTestLog("done retrying " + commandName);
            return kNoRetry;
        },
        () => "Timed out while retrying command '" + tojson(commandObj) +
            "', response: " + tojson(commandResponse),
        kRetryTimeout,
        kIntervalBetweenRetries);

    return commandResponse;
}

function runCommandWithRetryUponMigration(
    conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    const kQueryCommands = ['find', 'aggregate', 'listIndexes'];

    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    const inTransaction = commandObj.hasOwnProperty("autocommit");

    if (!inTransaction && kQueryCommands.includes(commandName)) {
        return _runAndExhaustQueryWithRetryUponMigration(
            conn, commandName, commandObj, func, makeFuncArgs);
    } else {
        // The expectation for tests running under this override method is that they cannot issue a
        // 'getMore' request, since any cursor-generating request should be served through
        // _runAndExhaustQueryWithRetryUponMigration().
        assert(commandName !== 'getMore' || inTransaction, 'Unexpected getMore received');
        return _runCommandWithRetryUponMigration(conn, commandName, commandObj, func, makeFuncArgs);
    }
}

OverrideHelpers.overrideRunCommand(runCommandWithRetryUponMigration);
