/**
 * Test that a secondary hangs up on an unacknowledged write.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

function getNotPrimaryUnackWritesCounter() {
    return assert.commandWorked(primaryDB.adminCommand({serverStatus: 1})).metrics.repl.network
        .notPrimaryUnacknowledgedWrites;
}

const collName = "not_primary_unacknowledged_write";

let rst = new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}]});
rst.startSet();
rst.initiate();
let primary = rst.getPrimary();
let secondary = rst.getSecondary();
var primaryDB = primary.getDB("test");
let secondaryDB = secondary.getDB("test");
let primaryColl = primaryDB[collName];
let secondaryColl = secondaryDB[collName];

// Verify that reading from secondaries does not impact `notPrimaryUnacknowledgedWrites`.
const preReadingCounter = getNotPrimaryUnackWritesCounter();
jsTestLog("Reading from secondary ...");
[
    {name: "findOne", fn: () => secondaryColl.findOne()},
    {name: "distinct", fn: () => secondaryColl.distinct("item")},
    {name: "count", fn: () => secondaryColl.find().count()},
].map(({name, fn}) => {
    assert.doesNotThrow(fn);
    assert.eq(assert.commandWorked(secondary.getDB("admin").hello()).isWritablePrimary, false);
});
const postReadingCounter = getNotPrimaryUnackWritesCounter();
assert.eq(preReadingCounter, postReadingCounter);

jsTestLog("Primary on port " + primary.port + " hangs up on unacknowledged writes");
// Do each write method with unacknowledged write concern, "wc".
[
    {name: "insertOne", fn: (wc) => secondaryColl.insertOne({}, wc)},
    {name: "insertMany", fn: (wc) => secondaryColl.insertMany([{}], wc)},
    {name: "deleteOne", fn: (wc) => secondaryColl.deleteOne({}, wc)},
    {name: "deleteMany", fn: (wc) => secondaryColl.deleteMany({}, wc)},
    {name: "updateOne", fn: (wc) => secondaryColl.updateOne({}, {$set: {x: 1}}, wc)},
    {name: "updateMany", fn: (wc) => secondaryColl.updateMany({}, {$set: {x: 1}}, wc)},
    {name: "replaceOne", fn: (wc) => secondaryColl.replaceOne({}, {}, wc)},
].map(({name, fn}) => {
    let result = assert.throws(
        function () {
            // Provoke the server to hang up.
            fn({writeConcern: {w: 0}});
            // The connection is now broken and hello() throws a network error.
            secondary.getDB("admin").hello();
        },
        [],
        "network error from " + name,
    );

    assert.includes(result.toString(), "network error while attempting to run command 'hello'", "after " + name);
});

// Unacknowledged write in progress when a stepdown occurs provokes a hangup.
assert.commandWorked(
    primaryDB.adminCommand({
        configureFailPoint: "hangAfterCollectionInserts",
        mode: "alwaysOn",
        data: {collectionNS: primaryColl.getFullName()},
    }),
);

let command = `
      checkLog.contains(db.getMongo(), "hangAfterCollectionInserts fail point enabled");
      db.adminCommand({replSetStepDown: 60, force: true});`;

let awaitShell = startParallelShell(command, primary.port);

let failedUnackWritesBefore = getNotPrimaryUnackWritesCounter();

jsTestLog("Beginning unacknowledged insert");
primaryColl.insertOne({}, {writeConcern: {w: 0}});

jsTestLog("Step down primary on port " + primary.port);
awaitShell({checkExitSuccess: false});

jsTestLog("Unacknowledged insert during stepdown provoked disconnect");
let result = assert.throws(
    function () {
        primary.getDB("admin").hello();
    },
    [],
    "network",
);
assert.includes(result.toString(), "network error while attempting to run command 'hello'");

// Validate the number of unacknowledged writes failed due to step down resulted in network
// disconnection.
let failedUnackWritesAfter = getNotPrimaryUnackWritesCounter();
assert.eq(failedUnackWritesAfter, failedUnackWritesBefore + 1);

rst.stopSet();
