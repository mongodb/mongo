// Test the explain command on the primary and on secondaries:
//
// 1) Explain of read operations should work on the secondaries iff secondaryOk is set.
//
// 2) Explain of write operations should
//     --fail on secondaries, even if secondaryOk is set,
//     --succeed on primary without applying any writes.

import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "explain_secondaryok";

print("Start replica set with two nodes");
let replTest = new ReplSetTest({name: name, nodes: 2});
let nodes = replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();

// Insert a document and let it sync to the secondary.
print("Initial sync");
primary.getDB("test").explain_secondaryok.insert({a: 1});
replTest.awaitReplication();

// Check that the document is present on the primary.
assert.eq(1, primary.getDB("test").explain_secondaryok.findOne({a: 1})["a"]);

// We shouldn't be able to read from the secondary with secondaryOk off.
let secondary = replTest.getSecondary();
secondary.getDB("test").getMongo().setSecondaryOk(false);
assert.throws(function () {
    secondary.getDB("test").explain_secondaryok.findOne({a: 1});
});

// With secondaryOk on, we should be able to read from the secondary.
secondary.getDB("test").getMongo().setSecondaryOk();
assert.eq(1, secondary.getDB("test").explain_secondaryok.findOne({a: 1})["a"]);

//
// Test explains on primary.
//

// Explain a count on the primary.
let explainOut = primary
    .getDB("test")
    .runCommand({explain: {count: "explain_secondaryok", query: {a: 1}}, verbosity: "executionStats"});
assert.commandWorked(explainOut, "explain read op on primary");

// Explain an update on the primary.
explainOut = primary.getDB("test").runCommand({
    explain: {update: "explain_secondaryok", updates: [{q: {a: 1}, u: {$set: {a: 5}}}]},
    verbosity: "executionStats",
});
assert.commandWorked(explainOut, "explain write op on primary");

// Plan should have an update stage at its root, reporting that it would
// modify a single document.
let stages = explainOut.executionStats.executionStages;
assert.eq("UPDATE", stages.stage);
assert.eq(1, stages.nWouldModify);

// Confirm that the document did not actually get modified on the primary
// or on the secondary.
assert.eq(1, primary.getDB("test").explain_secondaryok.findOne({a: 1})["a"]);
secondary.getDB("test").getMongo().setSecondaryOk();
assert.eq(1, secondary.getDB("test").explain_secondaryok.findOne({a: 1})["a"]);

//
// Test explains on secondary.
//

// Explain a count on the secondary with secondaryOk off. Should fail because
// secondaryOk is required for explains on a secondary.
secondary.getDB("test").getMongo().setSecondaryOk(false);
explainOut = secondary
    .getDB("test")
    .runCommand({explain: {count: "explain_secondaryok", query: {a: 1}}, verbosity: "executionStats"});
assert.commandFailed(explainOut, "explain read op on secondary, secondaryOk false");

// Explain of count should succeed once secondaryOk is true.
secondary.getDB("test").getMongo().setSecondaryOk();
explainOut = secondary
    .getDB("test")
    .runCommand({explain: {count: "explain_secondaryok", query: {a: 1}}, verbosity: "executionStats"});
assert.commandWorked(explainOut, "explain read op on secondary, secondaryOk true");

// Explain .find() on a secondary, setting secondaryOk directly on the query.
secondary.getDB("test").getMongo().setSecondaryOk(false);
assert.throws(function () {
    secondary.getDB("test").explain_secondaryok.explain("executionStats").find({a: 1}).finish();
});

secondary.getDB("test").getMongo().setSecondaryOk(false);
explainOut = secondary
    .getDB("test")
    .explain_secondaryok.explain("executionStats")
    .find({a: 1})
    .addOption(DBQuery.Option.slaveOk)
    .finish();
assert.commandWorked(explainOut, "explain read op on secondary, slaveOk bit set to true on query");

secondary.getDB("test").getMongo().setSecondaryOk();
explainOut = secondary.getDB("test").explain_secondaryok.explain("executionStats").find({a: 1}).finish();
assert.commandWorked(explainOut, "explain .find() on secondary, secondaryOk set to true");

// Explain .find() on a secondary, setting secondaryOk to false with various read preferences.
let readPrefModes = ["secondary", "secondaryPreferred", "primaryPreferred", "nearest"];
readPrefModes.forEach(function (prefString) {
    secondary.getDB("test").getMongo().setSecondaryOk(false);
    explainOut = secondary
        .getDB("test")
        .explain_secondaryok.explain("executionStats")
        .find({a: 1})
        .readPref(prefString)
        .finish();
    assert.commandWorked(explainOut, "explain .find() on secondary, '" + prefString + "' read preference on query");

    // Similarly should succeed if a read preference is set on the connection.
    secondary.setReadPref(prefString);
    explainOut = secondary.getDB("test").explain_secondaryok.explain("executionStats").find({a: 1}).finish();
    assert.commandWorked(
        explainOut,
        "explain .find() on secondary, '" + prefString + "' read preference on connection",
    );
    // Unset read pref on the connection.
    secondary.setReadPref();
});

// Fail explain find() on a secondary, setting secondaryOk to false with read preference set to
// primary.
let prefStringPrimary = "primary";
secondary.getDB("test").getMongo().setSecondaryOk(false);
explainOut = secondary
    .getDB("test")
    .runCommand({explain: {find: "explain_secondaryok", query: {a: 1}}, verbosity: "executionStats"});
assert.commandFailed(explainOut, "not primary and secondaryOk=false");

// Similarly should fail if a read preference is set on the connection.
secondary.setReadPref(prefStringPrimary);
explainOut = secondary
    .getDB("test")
    .runCommand({explain: {find: "explain_secondaryok", query: {a: 1}}, verbosity: "executionStats"});
assert.commandFailed(explainOut, "not primary and secondaryOk=false");
// Unset read pref on the connection.
secondary.setReadPref();

// Explain an update on the secondary with secondaryOk off. Should fail because
// secondaryOk is required for explains on a secondary.
secondary.getDB("test").getMongo().setSecondaryOk(false);
explainOut = secondary.getDB("test").runCommand({
    explain: {update: "explain_secondaryok", updates: [{q: {a: 1}, u: {$set: {a: 5}}}]},
    verbosity: "executionStats",
});
assert.commandFailed(explainOut, "explain write op on secondary, secondaryOk false");

// Explain of the update should also fail with secondaryOk on.
secondary.getDB("test").getMongo().setSecondaryOk();
explainOut = secondary.getDB("test").runCommand({
    explain: {update: "explain_secondaryok", updates: [{q: {a: 1}, u: {$set: {a: 5}}}]},
    verbosity: "executionStats",
});
assert.commandFailed(explainOut, "explain write op on secondary, secondaryOk true");
replTest.stopSet();
