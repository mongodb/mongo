/**
 * Overrides runCommand so operations that encounter the various error codes automatically retry,
 * when run in FCV upgrade/downgrade.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {runningWithViewlessTimeseriesUpgradeDowngrade} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

const kRetryTimeout = 10 * 60 * 1000;
const kIntervalBetweenRetries = 50;

// The following read operations defined in the CRUD specification are retryable.
// Note that estimatedDocumentCount() and countDocuments() use the count command.
const kRetryableReadCommands = new Set(["find", "aggregate", "distinct", "count", "explain"]);

// These are the commands that will retry on ConflictingOperationInProgress error codes.
const kRetryableCommands = new Set([
    // For $querySettings retry on 'queryShapeRepresentativeQueries' collection drop.
    "setQuerySettings",
    "removeQuerySettings",
]);

const kQueryRetryableErrors = [
    ErrorCodes.ConflictingOperationInProgress,
    ErrorCodes.QueryPlanKilled,
    // TODO SERVER-117477 stop retrying this error once we will not perform anymore
    // timeseries tranformation during FCV upgrade/downgrade
    ErrorCodes.InterruptedDueToTimeseriesUpgradeDowngrade,
];

function _runCommandWithRetry(conn, commandName, commandObj, func, makeFuncArgs) {
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

            // This handles the retry case when run against a standalone, replica set, or mongos
            // where both shards returned the same response.
            if (kQueryRetryableErrors.includes(commandResponse.code)) {
                jsTest.log.info(
                    `Retrying the '${commandName}' command because a conflicting operation is in progress: (attempt ${attempt})`,
                    {commandObj, commandResponse},
                );
                return kRetry;
            }

            jsTest.log.info(
                `Command '${commandName}' failed with error code: ${commandResponse.code} and will not be retried.`,
                {commandObj, commandResponse},
            );
            return kNoRetry;
        },
        () =>
            `Timed out while retrying command '${toJsonForLog(commandObj)}', response: ${toJsonForLog(
                commandResponse,
            )}`,
        kRetryTimeout,
        kIntervalBetweenRetries,
    );

    return commandResponse;
}

// TODO SERVER-117477: remove special handling of TimeseriesInterruptedDueToTimeseriesUpgradeDowngrade once 9.0 becomes last LTS
function _runCommandWithTimeseriesErrorInterception(conn, commandName, commandObj, func, makeFuncArgs) {
    let cmdResponse = func.apply(conn, makeFuncArgs(commandObj));

    if (
        (cmdResponse.hasOwnProperty("code") &&
            cmdResponse.code === ErrorCodes.InterruptedDueToTimeseriesUpgradeDowngrade) ||
        (cmdResponse.hasOwnProperty("writeErrors") &&
            cmdResponse.writeErrors.some((werr) => werr.code === ErrorCodes.InterruptedDueToTimeseriesUpgradeDowngrade))
    ) {
        jsTest.log.info(
            `Intercepted InterruptedDueToTimeseriesUpgradeDowngrade on command` +
                ` ${commandName} in a test suite with FCV upgrade/downgrade and no retryable` +
                ` writes. Skipping test execution.` +
                ` Command response: ${tojson(cmdResponse)}` +
                ` Original command: ${tojson(commandObj)}`,
        );
        quit();
    }

    return cmdResponse;
}

let _isRetryableWritesSuiteCached = null;
function isRetryableWritesSuiteCached(conn) {
    if (_isRetryableWritesSuiteCached === null) {
        _isRetryableWritesSuiteCached = OverrideHelpers.withPreOverrideRunCommand(() =>
            conn.getDB("admin").getSession().getOptions().shouldRetryWrites(),
        );
    }
    return _isRetryableWritesSuiteCached;
}

let _runningWithViewlessTimeseriesUpgradeDowngradeCached = null;
function runningWithViewlessTimeseriesUpgradeDowngradeCached(conn) {
    if (_runningWithViewlessTimeseriesUpgradeDowngradeCached === null) {
        _runningWithViewlessTimeseriesUpgradeDowngradeCached = OverrideHelpers.withPreOverrideRunCommand(() =>
            runningWithViewlessTimeseriesUpgradeDowngrade(conn.getDB("admin")),
        );
    }
    return _runningWithViewlessTimeseriesUpgradeDowngradeCached;
}

function runCommandWithRetry(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null) {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    if (kRetryableCommands.has(commandName) || kRetryableReadCommands.has(commandName)) {
        return _runCommandWithRetry(conn, commandName, commandObj, func, makeFuncArgs);
    }

    // TODO SERVER-117477: remove special handling of TimeseriesInterruptedDueToTimeseriesUpgradeDowngrade once 9.0 becomes last LTS
    if (!isRetryableWritesSuiteCached(conn) && runningWithViewlessTimeseriesUpgradeDowngradeCached(conn)) {
        return _runCommandWithTimeseriesErrorInterception(conn, commandName, commandObj, func, makeFuncArgs);
    }

    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.overrideRunCommand(runCommandWithRetry);
