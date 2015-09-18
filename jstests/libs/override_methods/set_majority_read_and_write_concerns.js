/**
 * Use prototype overrides to set a read concern of "majority" and a write concern of "majority"
 * while running core tests.
 */
(function() {
    "use strict";
    var defaultWriteConcern = {w: "majority", wtimeout: 60000};

    var originalStartParallelShell = startParallelShell;
    startParallelShell = function(jsCode, port, noConnect) {
        var newCode;
        var overridesFile = "jstests/libs/override_methods/set_majority_read_and_write_concerns.js";

        if (typeof(jsCode) === "function") {
            // Load the override file and immediately invoke the supplied function.
            newCode = `load("${overridesFile}"); (${jsCode})();`
        } else {
            newCode = `load("${overridesFile}"); ${jsCode};`
        }

        return originalStartParallelShell(newCode, port, noConnect);
    }

    DB.prototype._runCommandImpl = function(name, obj, options) {
        if (obj.hasOwnProperty("createIndexes") ||
            obj.hasOwnProperty("delete") ||
            obj.hasOwnProperty("findAndModify") ||
            obj.hasOwnProperty("findandmodify") ||
            obj.hasOwnProperty("insert") ||
            obj.hasOwnProperty("update")) {
            if (obj.hasOwnProperty("writeConcern")) {
                jsTestLog("Warning: overriding existing writeConcern of: " +
                           obj.writeConcern);
            }
            obj.writeConcern = defaultWriteConcern;

        } else if (obj.hasOwnProperty("aggregate") ||
            obj.hasOwnProperty("count") ||
            obj.hasOwnProperty("dbStats") ||
            obj.hasOwnProperty("distinct") ||
            obj.hasOwnProperty("explain") ||
            obj.hasOwnProperty("find") ||
            obj.hasOwnProperty("geoNear") ||
            obj.hasOwnProperty("geoSearch") ||
            obj.hasOwnProperty("group")) {
            if (obj.hasOwnProperty("readConcern")) {
                jsTestLog("Warning: overriding existing readConcern of: " +
                           obj.readConcern);
            }
            obj.readConcern = {level: "majority"};
        }

        return this.getMongo().runCommand(name, obj, options);
    };

    // Use a majority write concern if the operation does not specify one.
    DBCollection.prototype.getWriteConcern = function() {
        return new WriteConcern(defaultWriteConcern);
    };

})();

