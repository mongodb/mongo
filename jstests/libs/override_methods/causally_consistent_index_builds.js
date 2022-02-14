/**
 * Overrides runCommand so that background index builds are causally consistent.
 * TODO: SERVER-38961 This override is not necessary when two-phase index builds are complete.
 */
(function() {
"use strict";

load("jstests/libs/override_methods/override_helpers.js");

// This override runs a collMod after a createIndexes command. After collMod completes successfully
// we can guarantee the background index build started earlier has also completed. We update the
// command response operationTime and $clusterTime so causally consistent reads only read from
// that point onwards.
function runCommandWithCollMod(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    if (typeof commandObj !== "object" || commandObj === null || commandName !== "createIndexes") {
        return func.apply(conn, makeFuncArgs(commandObj));
    }

    // If checking for index build completion fails due to failover, stepdown, or concurrent drop
    // before we can check to see if it's done, then this retry loop should be able to restart and
    // hopefully finish the index build. This also makes it more likely that an error returned is
    // one resulting from a createIndex command instead of a collMod command. The user was trying
    // to do a createIndex command and is not anticipating errors from collMod.
    let res;
    let collModRes;
    let needRetry = true;
    const retryFailedMsg = () =>
        "can't verify index build completed. collMod failed after sending createIndex command: " +
        tojson(collModRes);
    for (let retry = 0; retry < 10 && needRetry; retry++) {
        res = func.apply(conn, makeFuncArgs(commandObj));
        if (!res.ok) {
            return res;
        }
        needRetry = false;

        let collModCmd = {collMod: commandObj[commandName]};
        assert.soon(() => {
            collModRes = func.apply(conn, makeFuncArgs(collModCmd));
            if (!collModRes.ok &&
                collModRes.code !== ErrorCodes.BackgroundOperationInProgressForNamespace) {
                needRetry = true;
                return true;
            }
            return collModRes.ok;
        }, retryFailedMsg);
    }

    // We ran out of retries and the collMod still failed. We can't guarantee the index build
    // completed so we unfortunately raise an error which the caller may have not been expecting.
    assert.commandWorked(collModRes, retryFailedMsg());

    // Overwrite the createIndex command's operation and cluster times, so that the owning
    // session can perform causal reads.
    if (collModRes.hasOwnProperty("operationTime")) {
        res.operationTime = collModRes["operationTime"];
    }
    if (collModRes.hasOwnProperty("$clusterTime")) {
        res.$clusterTime = collModRes["$clusterTime"];
    }
    return res;
}

OverrideHelpers.overrideRunCommand(runCommandWithCollMod);
})();
