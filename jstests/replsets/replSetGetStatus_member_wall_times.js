/**
 * Tests that replSetGetStatus responses include the last applied and durable wall times for other
 * members.
 *
 * @tags: [multiversion_incompatible]
 */

(function() {
"use strict";
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/rslib.js");

// We use GTE to account for the possibility of other writes in the system (e.g. HMAC).
// Comparison is GTE by default, GT if 'strict' is specified.
function checkWallTimes(primary, greaterMemberIndex, lesserMemberIndex, strict = false) {
    let res = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
    assert(res.members, () => tojson(res));

    const greater = res.members[greaterMemberIndex];
    assert(greater, () => tojson(res));
    const greaterApplied = greater.lastAppliedWallTime;
    const greaterDurable = greater.lastAppliedWallTime;
    assert(greaterApplied, () => tojson(res));
    assert(greaterDurable, () => tojson(res));

    const lesser = res.members[lesserMemberIndex];
    assert(lesser, () => tojson(res));
    const lesserApplied = lesser.lastAppliedWallTime;
    const lesserDurable = lesser.lastDurableWallTime;
    assert(lesser.lastAppliedWallTime, () => tojson(res));
    assert(lesser.lastDurableWallTime, () => tojson(res));

    if (!strict) {
        assert.gte(greaterApplied, lesserApplied, () => tojson(res));
        assert.gte(greaterDurable, lesserDurable, () => tojson(res));
    } else {
        assert.gt(greaterApplied, lesserApplied, () => tojson(res));
        assert.gt(greaterDurable, lesserDurable, () => tojson(res));
    }
}

const name = jsTestName();
const rst = new ReplSetTest({name: name, nodes: 3, settings: {chainingAllowed: false}});

rst.startSet();
rst.initiateWithHighElectionTimeout();
rst.awaitReplication();

const primary = rst.getPrimary();                                   // node 0
const [caughtUpSecondary, laggedSecondary] = rst.getSecondaries();  // nodes 1 and 2

const dbName = "testdb";
const collName = "testcoll";
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

jsTestLog("Creating test collection");
assert.commandWorked(primaryColl.insert({"one": 1}));
rst.awaitReplication();

checkWallTimes(primary, 0 /* greater */, 1 /* lesser */);
checkWallTimes(primary, 0 /* greater */, 2 /* lesser */);

jsTestLog("Stopping replication on secondary: " + laggedSecondary.host);
stopServerReplication(laggedSecondary);

jsTestLog("Adding more documents to collection");
assert.commandWorked(primaryColl.insert({"two": 2}, {writeConcern: {w: 1}}));
rst.awaitReplication(
    undefined /* timeout */, undefined /* secondaryOpTimeType */, [caughtUpSecondary]);

// Wall times of the lagged secondary should be strictly lesser.
checkWallTimes(primary, 0 /* greater */, 2 /* lesser */, true /* strict */);
checkWallTimes(primary, 1 /* greater */, 2 /* lesser */, true /* strict */);

jsTestLog("Letting lagged secondary catch up");
restartServerReplication(laggedSecondary);
rst.awaitReplication();

checkWallTimes(primary, 0 /* greater */, 1 /* lesser */);
checkWallTimes(primary, 0 /* greater */, 2 /* lesser */);

rst.stopSet();
})();
