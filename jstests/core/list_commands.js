// Test for listCommands.
// @tags: [multiversion_incompatible]

(function() {
"use strict";

var commands = db.runCommand({listCommands: 1});
assert.commandWorked(commands);

// Test that result is sorted.
function isSorted(obj) {
    var previousProperty;
    for (var property in obj["commands"]) {
        if (previousProperty && (previousProperty > property)) {
            return false;
        }
        previousProperty = property;
    }
    return true;
}
assert(isSorted(commands));

// Test that result contains basic commands.
assert(commands.hasOwnProperty("commands"));
assert(commands["commands"].hasOwnProperty("hello"));
assert(commands["commands"].hasOwnProperty("isMaster"));
assert(commands["commands"].hasOwnProperty("insert"));
assert(commands["commands"].hasOwnProperty("ping"));

// Test that commands listed have required properties
const hello = commands["commands"]["hello"];
assert(hello.hasOwnProperty("help"));
assert(hello.hasOwnProperty("slaveOk"));
assert(hello.hasOwnProperty("adminOnly"));
assert(hello.hasOwnProperty("requiresAuth"));

// Test that commands listed have required properties
const isMaster = commands["commands"]["isMaster"];
assert(isMaster.hasOwnProperty("help"));
assert(isMaster.hasOwnProperty("slaveOk"));
assert(isMaster.hasOwnProperty("adminOnly"));
assert(isMaster.hasOwnProperty("requiresAuth"));

// Test that requiresAuth outputs correct value
const insert = commands["commands"]["insert"];
assert(hello["requiresAuth"] === false);
assert(isMaster["requiresAuth"] === false);
assert(insert["requiresAuth"] === true);
})();
