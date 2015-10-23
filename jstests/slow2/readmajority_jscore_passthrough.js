/**
 * Use prototype overrides to set a read concern of "majority" and a write concern of "majority" 
 * while running core tests.
 */
(function() {
    "use strict";
    var defaultWriteConcern = {w: "majority", wtimeout: 60000};

    DB.prototype._runCommandImpl = function(name, obj, options) {
        var indexWait = false;
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
                // SERVER-20260 workaround
                if (obj.hasOwnProperty("createIndexes")) {
                    indexWait = true;
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
                    obj.readConcern = {level: "majority"};
                }

        var res = this.getMongo().runCommand(name, obj, options);
        if (indexWait) {
            print("Sleeping as workaround for SERVER-20260");
            sleep(5*1000);
        }
        return res;
    };


    // Use a majority write concern if the operation does not specify one.
    DBCollection.prototype.getWriteConcern = function() {
        return new WriteConcern(defaultWriteConcern);
    };

})();

