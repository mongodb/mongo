/**
 * The OverrideHelpers object defines convenience methods for overriding commands and functions in
 * the mongo shell.
 */
var OverrideHelpers = (function() {
    "use strict";

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
        const DBQueryOriginal = DBQuery;
        const mongoRunCommandOriginal = Mongo.prototype.runCommand;
        const mongoRunCommandWithMetadataOriginal = Mongo.prototype.runCommandWithMetadata;

        DBQuery = function(
            mongo, db, collection, ns, query, fields, limit, skip, batchSize, options) {
            // If the query isn't being run against the "$cmd" or "$cmd.sys" namespaces, then it
            // represents an OP_QUERY find on that collection. We skip calling overrideFunc() in
            // this case because the operation doesn't represent a command.
            if (!(collection instanceof DBCollection &&
                  (collection.getName() === "$cmd" || collection.getName().startsWith("$cmd.")))) {
                return DBQueryOriginal.apply(this, arguments);
            }

            // Due to the function signatures of Mongo.prototype.runCommand() and
            // Mongo.prototype.runCommandWithMetadata(), the overrideFunc() function expects that
            // the Mongo connection object is passed as the first argument and also represents the
            // 'this' parameter. As a workaround, we bind the appropriate 'this' value to the
            // DBQueryOriginal constructor ahead of time.
            const commandName = Object.keys(query)[0];
            return overrideFunc(
                mongo,
                db.getName(),
                commandName,
                query,
                DBQueryOriginal.bind(this),
                (query) =>
                    [mongo, db, collection, ns, query, fields, limit, skip, batchSize, options]);
        };

        // Copy any properties (e.g. DBQuery.Option) that are set on DBQueryOriginal.
        Object.keys(DBQueryOriginal).forEach(function(key) {
            DBQuery[key] = DBQueryOriginal[key];
        });

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
        isAggregationWithOutStage: isAggregationWithOutStage,
        isMapReduceWithInlineOutput: isMapReduceWithInlineOutput,
        prependOverrideInParallelShell: prependOverrideInParallelShell,
        overrideRunCommand: overrideRunCommand,
    };
})();
