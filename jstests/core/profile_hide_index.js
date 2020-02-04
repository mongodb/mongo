/**
 * Ensure the 'hidden' flag can be found in currentOp and profiler.
 *
 * @tags: [
 *      assumes_read_concern_unchanged,
 *      assumes_read_preference_unchanged,
 *      requires_non_retryable_writes,
 *      requires_profiling,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.
load("jstests/libs/profiler.js");                  // For profilerHasSingleMatchingEntryOrThrow.

const collName = "profile_hide_index";
const coll = assertDropAndRecreateCollection(db, collName);

function setPostCommandFailpoint({mode, options}) {
    assert.commandWorked(db.adminCommand(
        {configureFailPoint: "waitAfterCommandFinishesExecution", mode: mode, data: options}));
}

// This helper function is used to test 'hidden' flag can be found in 'currentOp' by given a command
// with 'hidden' option and a callback function to validate that the expected operations are in
// 'currentOp'.
function testHiddenFlagInCurrentOp({runCmd, isCommandExpected, cmdName}) {
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndex({b: 1}));

    let awaitHideIndex;
    try {
        setPostCommandFailpoint(
            {mode: "alwaysOn", options: {ns: coll.getFullName(), commands: [cmdName]}});

        awaitHideIndex = startParallelShell(runCmd);

        assert.soon(
            function() {
                const inprogs =
                    db.getSiblingDB("admin")
                        .aggregate([{$currentOp: {}}, {$match: {"ns": "test.profile_hide_index"}}])
                        .toArray();
                return inprogs.length > 0 && isCommandExpected(inprogs);
            },
            function() {
                return "Failed to find expected hidden_index command in currentOp()";
            });

    } finally {
        // Ensure that we unset the failpoint, regardless of the outcome of the test.
        setPostCommandFailpoint({mode: "off", options: {}});
    }

    awaitHideIndex();
    assert.commandWorked(coll.dropIndexes());
}

// Tests that "hidden_index" collMod command showed up in currentOp.
testHiddenFlagInCurrentOp({
    runCmd: `assert.commandWorked(db.runCommand({
                "collMod": "profile_hide_index",
                "index": {"name": "b_1", "hidden": true}}));`,
    isCommandExpected: function(inprogs) {
        return inprogs.find(function(inprog) {
            return inprog.command.collMod === "profile_hide_index" &&
                inprog.command.index.hidden === true && inprog.command.index.name === "b_1";
        }) !== undefined;
    },
    cmdName: "collMod"
});

// Tests that createIndex command with 'hidden' set to true showed up in currentOp.
testHiddenFlagInCurrentOp({
    runCmd: `assert.commandWorked(db.runCommand({
                "createIndexes": "profile_hide_index",
                "indexes": [{"key" : {c: 1}, "name": "c_1", "hidden": true}]}))`,
    isCommandExpected: function(inprogs) {
        return inprogs.find(function(inprog) {
            return inprog.command.createIndexes === "profile_hide_index" &&
                inprog.command.indexes[0].hidden === true &&
                inprog.command.indexes[0].name == "c_1";
        }) !== undefined;
    },
    cmdName: "createIndexes"
});

//
// Tests that 'hidden_index' commands can be found in the profiler;
//
// Should turn off profiling before dropping system.profile collection.
db.setProfilingLevel(0);
db.system.profile.drop();
db.setProfilingLevel(2);

assert.commandWorked(coll.createIndex({b: 1}, {hidden: true}));
profilerHasSingleMatchingEntryOrThrow({
    profileDB: db,
    filter: {
        "ns": "test.profile_hide_index",
        "command.createIndexes": "profile_hide_index",
        "command.indexes.hidden": true
    }
});

assert.commandWorked(
    db.runCommand({"collMod": coll.getName(), "index": {"name": "b_1", "hidden": false}}));
profilerHasSingleMatchingEntryOrThrow({
    profileDB: db,
    filter: {
        "ns": "test.profile_hide_index",
        "command.collMod": "profile_hide_index",
        "command.index.hidden": false,
    }
});
})();
