import {getCommandName} from "jstests/libs/cmd_object_utils.js";

// Store the original 'runCommand' function before applying any overrides.
const preOverrideRunCommand = Mongo.prototype.runCommand;

/**
 * The OverrideHelpers object defines convenience methods for overriding commands and functions in
 * the mongo shell.
 */
export const OverrideHelpers = (function() {
    function makeIsAggregationWithFirstStage(stageName) {
        return function(commandName, commandObj) {
            if (commandName !== "aggregate" || typeof commandObj !== "object" ||
                commandObj === null) {
                return false;
            }

            if (!Array.isArray(commandObj.pipeline) || commandObj.pipeline.length === 0) {
                return false;
            }

            const firstStage = commandObj.pipeline[0];
            if (typeof firstStage !== "object" || firstStage === null) {
                return false;
            }

            return Object.keys(firstStage)[0] === stageName;
        };
    }

    function isAggregationWithOutOrMergeStage(commandName, commandObj) {
        if (commandName !== "aggregate" || typeof commandObj !== "object" || commandObj === null) {
            return false;
        }

        if (!Array.isArray(commandObj.pipeline) || commandObj.pipeline.length === 0) {
            return false;
        }

        const lastStage = commandObj.pipeline[commandObj.pipeline.length - 1];
        if (typeof lastStage !== "object" || lastStage === null) {
            return false;
        }

        const lastStageName = Object.keys(lastStage)[0];
        return lastStageName === "$out" || lastStageName === "$merge";
    }

    function isMapReduceWithInlineOutput(commandName, commandObj) {
        if ((commandName !== "mapReduce" && commandName !== "mapreduce") ||
            typeof commandObj !== "object" || commandObj === null) {
            return false;
        }

        if (typeof commandObj.out !== "object") {
            return false;
        }

        return commandObj.out.hasOwnProperty("inline");
    }

    function prependOverrideInParallelShell(overrideFile) {
        const startParallelShellOriginal = startParallelShell;

        startParallelShell = function(jsCode, port, noConnect) {
            let newCode;
            if (typeof jsCode === "function") {
                // Load the override file and immediately invoke the supplied function.
                if (jsCode.constructor.name === 'AsyncFunction') {
                    newCode = `await import("${overrideFile}"); await (${jsCode.toString()})();`;
                } else {
                    newCode = `await import("${overrideFile}"); (${jsCode.toString()})();`;
                }
            } else {
                newCode = `await import("${overrideFile}"); ${jsCode};`;
            }

            return startParallelShellOriginal(newCode, port, noConnect);
        };
    }

    function overrideRunCommand(overrideFunc) {
        const mongoRunCommandOriginal = Mongo.prototype.runCommand;

        Mongo.prototype.runCommand = function(dbName, commandObj, options) {
            return overrideFunc(this,
                                dbName,
                                getCommandName(commandObj),
                                commandObj,
                                mongoRunCommandOriginal,
                                (commandObj, db = dbName) => [db, commandObj, options]);
        };
    }

    /**
     * Higher order function for executing `db.runCommand()` commands without overrides.
     * Example usage:
     *  const res = OverrideHelpers.withPreOverrideRunCommand(() => db.adminCommand(cmd));
     */
    function withPreOverrideRunCommand(fn) {
        const overriddenRunCommand = Mongo.prototype.runCommand;
        try {
            Mongo.prototype.runCommand = preOverrideRunCommand;
            return fn();
        } finally {
            Mongo.prototype.runCommand = overriddenRunCommand;
        }
    }

    return {
        isAggregationWithListLocalSessionsStage:
            makeIsAggregationWithFirstStage("$listLocalSessions"),
        isAggregationWithOutOrMergeStage: isAggregationWithOutOrMergeStage,
        isAggregationWithCurrentOpStage: makeIsAggregationWithFirstStage("$currentOp"),
        isAggregationWithChangeStreamStage: makeIsAggregationWithFirstStage("$changeStream"),
        isAggregationWithInternalListCollections:
            makeIsAggregationWithFirstStage("$_internalListCollections"),
        isAggregationWithListClusterCatalog: makeIsAggregationWithFirstStage("$listClusterCatalog"),
        isMapReduceWithInlineOutput: isMapReduceWithInlineOutput,
        prependOverrideInParallelShell: prependOverrideInParallelShell,
        overrideRunCommand: overrideRunCommand,
        withPreOverrideRunCommand: withPreOverrideRunCommand,
    };
})();
