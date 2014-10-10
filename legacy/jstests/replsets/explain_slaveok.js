// Test the explain command on the primary and on secondaries:
//
// 1) Explain of read operations should work on the secondaries iff slaveOk is set.
//
// 2) Explain of write operations should
//     --fail on secondaries, even if slaveOk is set,
//     --succeed on primary without applying any writes.

var name = "explain_slaveok";

print("Start replica set with two nodes");
var replTest = new ReplSetTest({name: name, nodes: 2});
var nodes = replTest.startSet();
replTest.initiate();
var primary = replTest.getMaster();

// Insert a document and let it sync to the secondary.
print("Initial sync");
primary.getDB("test").explain_slaveok.insert({a: 1});
replTest.awaitReplication();

// Check that the document is present on the primary.
assert.eq(1, primary.getDB("test").explain_slaveok.findOne({a: 1})["a"]);

// We shouldn't be able to read from the secondary with slaveOk off.
var secondary = replTest.getSecondary();
secondary.getDB("test").getMongo().setSlaveOk(false);
assert.throws(function() {
    secondary.getDB("test").explain_slaveok.findOne({a: 1});
});

// With slaveOk on, we should be able to read from the secondary.
secondary.getDB("test").getMongo().setSlaveOk(true);
assert.eq(1, secondary.getDB("test").explain_slaveok.findOne({a: 1})["a"]);

//
// Test explains on primary.
//

// Explain a count on the primary.
var explainOut = primary.getDB("test").runCommand({
    explain: {
        count: "explain_slaveok",
        query: {a: 1}
    },
    verbosity: "executionStats"
});
printjson(explainOut);
assert.commandWorked(explainOut, "explain read op on primary");

// Explain an update on the primary.
explainOut = primary.getDB("test").runCommand({
    explain: {
        update: "explain_slaveok",
        updates: [
            {q: {a: 1}, u: {$set: {a: 5}}}
        ]
    },
    verbosity: "executionStats"
});
printjson(explainOut);
assert.commandWorked(explainOut, "explain write op on primary");

// Plan should have an update stage at its root, reporting that it would
// modify a single document.
var stages = explainOut.executionStats.executionStages;
assert.eq("UPDATE", stages.stage);
assert.eq(1, stages.nWouldModify);

// Confirm that the document did not actually get modified on the primary
// or on the secondary.
assert.eq(1, primary.getDB("test").explain_slaveok.findOne({a: 1})["a"]);
secondary.getDB("test").getMongo().setSlaveOk(true);
assert.eq(1, secondary.getDB("test").explain_slaveok.findOne({a: 1})["a"]);

//
// Test explains on secondary.
//

// Explain a count on the secondary with slaveOk off. Should fail because
// slaveOk is required for explains on a secondary.
secondary.getDB("test").getMongo().setSlaveOk(false);
explainOut = secondary.getDB("test").runCommand({
    explain: {
        count: "explain_slaveok",
        query: {a: 1}
    },
    verbosity: "executionStats"
});
printjson(explainOut);
assert.commandFailed(explainOut, "explain read op on secondary, slaveOk false");

// Explain of count should succeed once slaveOk is true.
secondary.getDB("test").getMongo().setSlaveOk(true);
explainOut = secondary.getDB("test").runCommand({
    explain: {
        count: "explain_slaveok",
        query: {a: 1}
    },
    verbosity: "executionStats"
});
printjson(explainOut);
assert.commandWorked(explainOut, "explain read op on secondary, slaveOk true");

// Explain an update on the secondary with slaveOk off. Should fail because
// slaveOk is required for explains on a secondary.
secondary.getDB("test").getMongo().setSlaveOk(false);
explainOut = secondary.getDB("test").runCommand({
    explain: {
        update: "explain_slaveok",
        updates: [
            {q: {a: 1}, u: {$set: {a: 5}}}
        ]
    },
    verbosity: "executionStats"
});
printjson(explainOut);
assert.commandFailed(explainOut, "explain write op on secondary, slaveOk false");

// Explain of the update should also fail with slaveOk on.
secondary.getDB("test").getMongo().setSlaveOk(true);
explainOut = secondary.getDB("test").runCommand({
    explain: {
        update: "explain_slaveok",
        updates: [
            {q: {a: 1}, u: {$set: {a: 5}}}
        ]
    },
    verbosity: "executionStats"
});
printjson(explainOut);
assert.commandFailed(explainOut, "explain write op on secondary, slaveOk true");
