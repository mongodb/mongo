/**
 * Use prototype overrides to set read preference to "secondary" when running tests.
 */
(function() {
    "use strict";

    const readPreferenceSecondary = {mode: "secondary"};

    db.getMongo().setReadPref("secondary");

    const originalStartParallelShell = startParallelShell;
    startParallelShell = function(jsCode, port, noConnect) {
        let newCode;
        const overridesFile = "jstests/libs/override_methods/set_read_preference_secondary.js";
        if (typeof(jsCode) === "function") {
            // Load the override file and immediately invoke the supplied function.
            newCode = `load("${overridesFile}"); (${jsCode})();`;
        } else {
            newCode = `load("${overridesFile}"); ${jsCode};`;
        }

        return originalStartParallelShell(newCode, port, noConnect);
    };

    // These are reading commands that support a read preference.
    const commandsToForceReadPreference = new Set([
        "aggregate",
        "collStats",
        "count",
        "dbStats",
        "distinct",
        "geoNear",
        "geoSearch",
        "group",
        "mapReduce",
        "mapreduce",
        "parallelCollectionScan",
    ]);

    const originalRunCommand = DB.prototype._runCommandImpl;
    DB.prototype._runCommandImpl = function(dbName, obj, options) {
        const cmdName = Object.keys(obj)[0];

        let forceReadPreference = commandsToForceReadPreference.has(cmdName);
        if (cmdName === "aggregate" && obj.pipeline && Array.isArray(obj.pipeline) &&
            obj.pipeline.length > 0 &&
            obj.pipeline[obj.pipeline.length - 1].hasOwnProperty("$out")) {
            forceReadPreference = false;
        } else if ((cmdName === "mapReduce" || cmdName === "mapreduce") &&
                   obj.hasOwnProperty("out") && typeof obj.out === "object" && !obj.out.inline) {
            forceReadPreference = false;
        }

        if (forceReadPreference) {
            if (obj.hasOwnProperty("$readPreference")) {
                if (bsonWoCompare(obj.$readPreference, readPreferenceSecondary) !== 0) {
                    jsTestLog("Warning: _runCommandImpl overriding existing $readPreference of: " +
                              tojson(obj.$readPreference));
                    obj.$readPreference = readPreferenceSecondary;
                }
            } else {
                obj.$readPreference = readPreferenceSecondary;
            }
        }

        return originalRunCommand.call(this, dbName, obj, options);
    };

})();
