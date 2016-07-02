/**
 * Use prototype overrides to set a read concern of "majority" and a write concern of "majority"
 * while running core tests.
 */
(function() {
    "use strict";
    var defaultWriteConcern = {
        w: "majority",
        // Use a "signature" value that won't typically match a value assigned in normal use.
        wtimeout: 60321
    };
    var defaultReadConcern = {level: "majority"};

    var originalDBQuery = DBQuery;

    DBQuery = function(mongo, db, collection, ns, query, fields, limit, skip, batchSize, options) {
        if (ns.endsWith("$cmd")) {
            if (query.hasOwnProperty("writeConcern") &&
                bsonWoCompare(query.writeConcern, defaultWriteConcern) !== 0) {
                jsTestLog("Warning: DBQuery overriding existing writeConcern of: " +
                          tojson(query.writeConcern));
                query.writeConcern = defaultWriteConcern;
            }
        }

        return originalDBQuery.apply(this, arguments);
    };

    DBQuery.Option = originalDBQuery.Option;

    var originalStartParallelShell = startParallelShell;
    startParallelShell = function(jsCode, port, noConnect) {
        var newCode;
        var overridesFile = "jstests/libs/override_methods/set_majority_read_and_write_concerns.js";

        if (typeof(jsCode) === "function") {
            // Load the override file and immediately invoke the supplied function.
            newCode = `load("${overridesFile}"); (${jsCode})();`;
        } else {
            newCode = `load("${overridesFile}"); ${jsCode};`;
        }

        return originalStartParallelShell(newCode, port, noConnect);
    };

    DB.prototype._runCommandImpl = function(dbName, obj, options) {
        var cmdName = "";
        for (var fieldName in obj) {
            cmdName = fieldName;
            break;
        }

        // These commands directly support a writeConcern argument.
        var commandsToForceWriteConcern = [
            "applyOps",
            "authSchemaUpgrade",
            "createRole",
            "createUser",
            "delete",
            "dropAllRolesFromDatabase",
            "dropAllUsersFromDatabase",
            "dropRole",
            "dropUser",
            "findAndModify",
            "findandmodify",
            "grantPrivilegesToRole",
            "grantRolesToRole",
            "grantRolesToUser",
            "insert",
            "revokeRolesFromRole",
            "revokeRolesFromUser",
            "update",
            "updateRole",
            "updateUser",
        ];

        // These commands do writes but do not support a writeConcern argument. Emulate it with a
        // getLastError command.
        var commandsToEmulateWriteConcern = [
            "createIndexes",
        ];

        // These are reading commands that support majority readConcern.
        var commandsToForceReadConcern = [
            "count",
            "distinct",
            "find",
            "geoNear",
            "geoSearch",
            "group",
        ];

        var forceWriteConcern = Array.contains(commandsToForceWriteConcern, cmdName);
        var emulateWriteConcern = Array.contains(commandsToEmulateWriteConcern, cmdName);
        var forceReadConcern = Array.contains(commandsToForceReadConcern, cmdName);

        if (cmdName === "aggregate") {
            // Aggregate can be either a read or a write depending on whether it has a $out stage.
            // $out is required to be the last stage of the pipeline.
            var stages = obj.pipeline;
            var hasOut = stages && (stages.length !== 0) && ('$out' in stages[stages.length - 1]);
            if (hasOut) {
                emulateWriteConcern = true;
            } else {
                forceReadConcern = true;
            }
        }

        if (forceWriteConcern) {
            if (obj.hasOwnProperty("writeConcern")) {
                if (bsonWoCompare(obj.writeConcern, defaultWriteConcern) !== 0) {
                    jsTestLog("Warning: _runCommandImpl overriding existing writeConcern of: " +
                              tojson(obj.writeConcern));
                    obj.writeConcern = defaultWriteConcern;
                }
            } else {
                obj.writeConcern = defaultWriteConcern;
            }

        } else if (forceReadConcern) {
            if (obj.hasOwnProperty("readConcern")) {
                if (bsonWoCompare(obj.readConcern, defaultReadConcern) !== 0) {
                    jsTestLog("Warning: _runCommandImpl overriding existing readConcern of: " +
                              tojson(obj.readConcern));
                    obj.readConcern = defaultReadConcern;
                }
            } else {
                obj.readConcern = defaultReadConcern;
            }
        }

        var res = this.getMongo().runCommand(dbName, obj, options);

        if (res.ok && emulateWriteConcern) {
            // We only emulate WriteConcern if the command succeeded to match the behavior of
            // commands that support WriteConcern.
            var gleCmd = Object.extend({getLastError: 1}, defaultWriteConcern);
            assert.commandWorked(this.getMongo().runCommand(dbName, gleCmd, options));
        }

        return res;
    };

    // Use a majority write concern if the operation does not specify one.
    DBCollection.prototype.getWriteConcern = function() {
        return new WriteConcern(defaultWriteConcern);
    };

})();
