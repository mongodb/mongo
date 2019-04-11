/**
 * Overrides runCommand so that background index builds are causally consistent.
 * TODO: SERVER-38961 This override is not necessary when two-phase index builds are complete.
 */
(function() {
    "use strict";

    load("jstests/libs/override_methods/override_helpers.js");

    // This override runs a collMod after a createIndexes command. After collMod completes
    // we can guarantee the background index build started earlier has also completed. We update the
    // command response operationTime and $clusterTime so causally consistent reads only read from
    // that point onwards.
    function runCommandWithCollMod(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
        if (typeof commandObj !== "object" || commandObj === null) {
            return func.apply(conn, makeFuncArgs(commandObj));
        }

        let res = func.apply(conn, makeFuncArgs(commandObj));
        if (commandName !== "createIndexes") {
            return res;
        }
        if (!res.ok) {
            return res;
        }

        let collModCmd = {collMod: commandObj[commandName]};
        let collModRes = func.apply(conn, makeFuncArgs(collModCmd));

        // If a follow-up collMod fails, another command was likely able to execute after the
        // createIndexes command. That means it is safe to use the latest operationTime for
        // causal consistency purposes.
        if (!collModRes.ok) {
            print('note: ignoring collMod failure after sending createIndex command: ' +
                  tojson(collModRes));
        }

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
