/**
 * Test that the mapReduce command fails gracefully when user-provided JavaScript code throws and
 * that the user gets back a JavaScript stacktrace.
 *
 * @tags: [
 *   # mapReduce does not support afterClusterTime.
 *   does_not_support_causal_consistency,
 *   does_not_support_stepdowns,
 *   requires_scripting,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

// Do not execute new path on the passthrough suites.
if (!FixtureHelpers.isMongos(db)) {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryUseAggMapReduce: true}));
}

let coll = db.mr_tolerates_js_exception;
coll.drop();
for (let i = 0; i < 100; i++) {
    assert.commandWorked(coll.insert({_id: i, a: 1}));
}

// Test that the command fails with a JS interpreter failure error when the reduce function
// throws.
let cmdOutput = db.runCommand({
    mapReduce: coll.getName(),
    map: function() {
        emit(this.a, 1);
    },
    reduce: function(key, value) {
        (function myFunction() {
            throw new Error("Intentionally thrown inside reduce function");
        })();
    },
    out: {inline: 1}
});
assert.commandFailedWithCode(cmdOutput, ErrorCodes.JSInterpreterFailure, tojson(cmdOutput));
assert(
    /Intentionally thrown inside reduce function/.test(cmdOutput.errmsg),
    () => "mapReduce didn't include the message from the exception thrown: " + tojson(cmdOutput));
assert(/myFunction@/.test(cmdOutput.errmsg),
       () => "mapReduce didn't return the JavaScript stacktrace: " + tojson(cmdOutput));
assert(!cmdOutput.hasOwnProperty("stack"),
       () => "mapReduce shouldn't return JavaScript stacktrace separately: " + tojson(cmdOutput));
assert(!cmdOutput.hasOwnProperty("originalError"),
       () => "mapReduce shouldn't return wrapped version of the error: " + tojson(cmdOutput));

// Test that the command fails with a JS interpreter failure error when the map function
// throws.
cmdOutput = db.runCommand({
    mapReduce: coll.getName(),
    map: function() {
        (function myFunction() {
            throw new Error("Intentionally thrown inside map function");
        })();
    },
    reduce: function(key, value) {
        return Array.sum(value);
    },
    out: {inline: 1}
});
assert.commandFailedWithCode(cmdOutput, ErrorCodes.JSInterpreterFailure, tojson(cmdOutput));
assert(
    /Intentionally thrown inside map function/.test(cmdOutput.errmsg),
    () => "mapReduce didn't include the message from the exception thrown: " + tojson(cmdOutput));
assert(/myFunction@/.test(cmdOutput.errmsg),
       () => "mapReduce didn't return the JavaScript stacktrace: " + tojson(cmdOutput));
assert(!cmdOutput.hasOwnProperty("stack"),
       () => "mapReduce shouldn't return JavaScript stacktrace separately: " + tojson(cmdOutput));
assert(!cmdOutput.hasOwnProperty("originalError"),
       () => "mapReduce shouldn't return wrapped version of the error: " + tojson(cmdOutput));

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryUseAggMapReduce: false}));
}());
