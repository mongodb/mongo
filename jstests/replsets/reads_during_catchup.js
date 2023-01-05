(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/rslib.js");

let name = TestData.testName;
let rst = new ReplSetTest({
    name: name,
    nodes: [{}, {}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    waitForKeys: true,
    settings: {
        heartbeatIntervalMillis: 500,
        electionTimeoutMillis: 10000,
        catchUpTimeoutMillis: 4 * 60 * 1000
    }
});

rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

const origPrimary = rst.getPrimary();
const origDb = origPrimary.getDB(name);
const origColl = origDb.coll;
const newPrimary = rst.nodes[1];
const extraNode1 = rst.nodes[2];
const extraNode2 = rst.nodes[3];
const dataStr = "data to read during catchup";
TestData.dataStr = dataStr;

assert.commandWorked(origColl.insert({_id: dataStr}));
rst.awaitReplication();
setLogVerbosity([newPrimary], {"storage": {"verbosity": 2}});

// Make some data non-majority-committed.
jsTestLog("Writing non-majority data");
stopServerReplication([extraNode1, extraNode2]);
assert.commandWorked(origColl.update({_id: dataStr}, {$set: {x: 1}}, {writeConcern: {w: 2}}));
jsTestLog("Writing data to give new primary something to catch up to.");
stopServerReplication([newPrimary]);
assert.commandWorked(origDb.catchupcoll.insert([{a: 1}, {a: 2}, {a: 3}], {writeConcern: {w: 1}}));
// Step up the new primary and leave it in catch-up mode
jsTestLog("Stepping up the new primary");
const newDb = newPrimary.getDB(name);
const newColl = newDb.coll;
rst.stepUp(newPrimary, {awaitReplicationBeforeStepUp: false, awaitWritablePrimary: false});
assert.eq(newPrimary.getDB("admin").hello().isWritablePrimary, false);
jsTestLog("Available read");
assert.docEq(newColl.find({}).readConcern("available").toArray(), [{_id: TestData.dataStr, x: 1}]);
jsTestLog("Local read");
assert.docEq(newColl.find({}).readConcern("local").toArray(), [{_id: TestData.dataStr, x: 1}]);
if (jsTest.options().enableMajorityReadConcern === false ||
    jsTest.options().storageEngine == "ephemeralForTest") {
    jsTestLog("Majority read (disabled, should fail)");
    assert.throwsWithCode(() => newColl.find({}).readConcern("majority").toArray(),
                          ErrorCodes.ReadConcernMajorityNotEnabled);
} else {
    jsTestLog("Majority read");
    assert.docEq(newColl.find({}).readConcern("majority").toArray(), [{_id: TestData.dataStr}]);
}
jsTestLog("Linearizable read");
assert.throwsWithCode(() => newColl.find({}).readConcern("linearizable").toArray(),
                      ErrorCodes.NotWritablePrimary);
jsTestLog("Reads complete");

// Allow the new primary to catch up.
restartServerReplication(rst.nodes);
rst.stopSet();
})();
