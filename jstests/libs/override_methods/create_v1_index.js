/**
 * This file defines command overrides to create v:1 indexes by default.
 */
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

function runCommandOverride(conn, dbName, cmdName, cmdObj, clientFunction, makeFuncArgs) {
    if (cmdName == "createIndexes") {
        cmdObj["indexes"].forEach(index => {
            // Ignore empty specs
            if (Object.keys(index).length == 0) {
                return;
            }
            // Avoid conflicts with the default _id index
            if (Object.keys(index["key"]).length == 1 && index["key"]["_id"] == 1) {
                return;
            }
            // v:1 does not support wildcards
            for (let key in index["key"]) {
                if (key.includes("$**")) {
                    return;
                }
            }
            // v:1 does not support collation
            if ("collation" in index) {
                return;
            }
            // If v is not specified, default to v:1
            if (!("v" in index)) {
                index["v"] = 1;
            }
        });
    }
    // Call the original function, with a potentially modified command object.
    return clientFunction.apply(conn, makeFuncArgs(cmdObj));
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);
