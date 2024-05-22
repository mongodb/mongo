import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

/**
 * Throws an error if this command should not be run in config transition suites.
 */
function checkCanRun(dbName, commandName, commandObj) {
    if (commandName == "setFeatureCompatibilityVersion" && commandObj[commandName] != latestFCV) {
        throw Error("Cannot downgrade the FCV since if the setFCV command fails partway through, " +
                    "the FCV may be left in the upgrading or downgrading state and the " +
                    "transitionFromDedicatedConfigServer command and moveCollection command are " +
                    "not supported when the FCV is upgrading or downgrading.");
    }
}

function runCommand(conn, dbName, commandName, commandObj, func, makeFuncArgs) {
    checkCanRun(dbName, commandName, commandObj);
    return func.apply(conn, makeFuncArgs(commandObj));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/config_transitions.js");

OverrideHelpers.overrideRunCommand(runCommand);
