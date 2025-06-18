/**
 * Overrides runCommand so operations that encounter the BackgroundOperationInProgressForNs/Db error
 * codes automatically retry.
 */

import {getCollectionNameFromFullNamespace} from "jstests/libs/namespace_utils.js";
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
                const ns = queryResponse.cursor.ns;
                const collName = getCollectionNameFromFullNamespace(ns);

                // Exhaust the cursor returned by the command and return to the test a single batch
                // containing all the results.
                const getMoreCommandObj = {
                    getMore: latestBatchResponse.cursor.id,
                    collection: collName,
                    lsid: lsid,
                    apiVersion: apiVersion,
                    apiStrict: apiStrict,
                };

                // We're not propagating the `txnNumber` parameter to the getMore command because
                // this function is never called when the query is part of a transaction.

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

function _runDDLCommandWithRetryUponMigration(conn, commandName, commandObj, func, makeFuncArgs) {
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

            let message = "Retrying the " + commandName +
                " command because a migration operation is in progress (attempt " + attempt +
                "): " + tojson(commandResponse);

            // This handles the retry case when run against a standalone, replica set, or mongos
            // where both shards returned the same response.
            if (kCommandRetryableErrors.includes(commandResponse.code)) {
                jsTestLog(message);
                return kRetry;
            }

            jsTestLog("Done retrying " + commandName);
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
    // These are the query commands that will be retried when failing due to a concurrent chunk or
    // collection migrations.
    const kQueryCommands =
        new Set(['find', 'aggregate', 'listIndexes', 'count', 'distinct', 'explain']);

    // These are the DDL commands that can return BackgroundOperationInProgress error codes due to
    // concurrent chunk or collection migrations.
    const kRetryableDDLCommands = new Set([
        "createIndexes",
        "moveCollection",
        "reshardCollection",
        "unshardCollection",
    ]);

    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    // A transaction can either be issued by the test file or injected by a suite override.
    const inTransaction = commandObj.hasOwnProperty("autocommit") ||
        (TestData.networkErrorAndTxnOverrideConfig &&
         TestData.networkErrorAndTxnOverrideConfig.wrapCRUDinTransactions);

    if (!inTransaction && kQueryCommands.has(commandName)) {
        return _runAndExhaustQueryWithRetryUponMigration(
            conn, commandName, commandObj, func, makeFuncArgs);

    } else if (kRetryableDDLCommands.has(commandName)) {
        return _runDDLCommandWithRetryUponMigration(
            conn, commandName, commandObj, func, makeFuncArgs);

    } else {
        return func.apply(conn, makeFuncArgs(commandObj));
    }
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicitly_retry_on_migration_in_progress.js");

OverrideHelpers.overrideRunCommand(runCommandWithRetryUponMigration);
