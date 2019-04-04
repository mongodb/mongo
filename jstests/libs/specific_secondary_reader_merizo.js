"use strict";

// This is a subclass of Mongo, which stores both the default connection
// and the direct connection to a specific secondary node. For reads with
// readPreference "secondary", they are sent to the specific secondary node.

function SpecificSecondaryReaderMongo(host, secondary) {
    var defaultMongo = new Mongo(host);
    var secondaryMongo = new Mongo(secondary);

    // This overrides the default runCommand() in Mongo
    this.runCommand = function runCommand(dbName, commandObj, options) {
        // If commandObj is specified with the readPreference "secondary", then use direct
        // connection to secondary. Otherwise use the default connection.
        if (commandObj.hasOwnProperty("$readPreference")) {
            if (commandObj.$readPreference.mode === "secondary") {
                return secondaryMongo.runCommand(dbName, commandObj, options);
            }
        }
        return defaultMongo.runCommand(dbName, commandObj, options);
    };

    return new Proxy(this, {
        get: function get(target, property, receiver) {
            // If the property is defined on the SpecificSecondaryReaderMongo instance itself, then
            // return it.  Otherwise, get the value of the property from the Mongo instance.
            if (target.hasOwnProperty(property)) {
                return target[property];
            }
            var value = defaultMongo[property];
            if (typeof value === "function") {
                if (property === "getDB" || property === "startSession") {
                    // 'receiver' is the Proxy object.
                    return value.bind(receiver);
                }
                return value.bind(defaultMongo);
            }
            return value;
        },

        set: function set(target, property, value, receiver) {
            // Delegate setting the value of any property to the Mongo instance so
            // that it can be accessed in functions acting on the Mongo instance
            // directly instead of this Proxy.  For example, the "slaveOk" property
            // needs to be set on the Mongo instance in order for the query options
            // bit to be set correctly.
            defaultMongo[property] = value;
            return true;
        },
    });
}
