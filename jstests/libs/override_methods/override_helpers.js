/**
 * The OverrideHelpers object defines convenience methods for overriding commands and functions in
 * the mongo shell.
 */
var OverrideHelpers = (function() {
    "use strict";

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

    function isAggregationWithOutStage(commandName, commandObj) {
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

        return Object.keys(lastStage)[0] === "$out";
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
                newCode = `load("${overrideFile}"); (${jsCode})();`;
            } else {
                newCode = `load("${overrideFile}"); ${jsCode};`;
            }

            return startParallelShellOriginal(newCode, port, noConnect);
        };
    }

    function overrideRunCommand(overrideFunc) {
        const mongoRunCommandOriginal = Mongo.prototype.runCommand;
        const mongoRunCommandWithMetadataOriginal = Mongo.prototype.runCommandWithMetadata;

        Mongo.prototype.runCommand = function(dbName, commandObj, options) {
            const commandName = Object.keys(commandObj)[0];
            return overrideFunc(this,
                                dbName,
                                commandName,
                                commandObj,
                                mongoRunCommandOriginal,
                                (commandObj) => [dbName, commandObj, options]);
        };

        Mongo.prototype.runCommandWithMetadata = function(dbName, metadata, commandArgs) {
            const commandName = Object.keys(commandArgs)[0];
            return overrideFunc(this,
                                dbName,
                                commandName,
                                commandArgs,
                                mongoRunCommandWithMetadataOriginal,
                                (commandArgs) => [dbName, metadata, commandArgs]);
        };
    }

    return {
        isAggregationWithListLocalCursorsStage:
            makeIsAggregationWithFirstStage("$listLocalCursors"),
        isAggregationWithListLocalSessionsStage:
            makeIsAggregationWithFirstStage("$listLocalSessions"),
        isAggregationWithOutStage: isAggregationWithOutStage,
        isMapReduceWithInlineOutput: isMapReduceWithInlineOutput,
        prependOverrideInParallelShell: prependOverrideInParallelShell,
        overrideRunCommand: overrideRunCommand,
    };
})();
