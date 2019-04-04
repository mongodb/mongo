"use strict";

// This is a subclass of Merizo, which stores both the default connection
// and the direct connection to a specific secondary node. For reads with
// readPreference "secondary", they are sent to the specific secondary node.

function SpecificSecondaryReaderMerizo(host, secondary) {
    var defaultMerizo = new Merizo(host);
    var secondaryMerizo = new Merizo(secondary);

    // This overrides the default runCommand() in Merizo
    this.runCommand = function runCommand(dbName, commandObj, options) {
        // If commandObj is specified with the readPreference "secondary", then use direct
        // connection to secondary. Otherwise use the default connection.
        if (commandObj.hasOwnProperty("$readPreference")) {
            if (commandObj.$readPreference.mode === "secondary") {
                return secondaryMerizo.runCommand(dbName, commandObj, options);
            }
        }
        return defaultMerizo.runCommand(dbName, commandObj, options);
    };

    return new Proxy(this, {
        get: function get(target, property, receiver) {
            // If the property is defined on the SpecificSecondaryReaderMerizo instance itself, then
            // return it.  Otherwise, get the value of the property from the Merizo instance.
            if (target.hasOwnProperty(property)) {
                return target[property];
            }
            var value = defaultMerizo[property];
            if (typeof value === "function") {
                if (property === "getDB" || property === "startSession") {
                    // 'receiver' is the Proxy object.
                    return value.bind(receiver);
                }
                return value.bind(defaultMerizo);
            }
            return value;
        },

        set: function set(target, property, value, receiver) {
            // Delegate setting the value of any property to the Merizo instance so
            // that it can be accessed in functions acting on the Merizo instance
            // directly instead of this Proxy.  For example, the "slaveOk" property
            // needs to be set on the Merizo instance in order for the query options
            // bit to be set correctly.
            defaultMerizo[property] = value;
            return true;
        },
    });
}
