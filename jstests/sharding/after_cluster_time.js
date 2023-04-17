/**
 * Tests readConcern: afterClusterTime behavior in a sharded cluster.
 * @tags: [requires_majority_read_concern]
 */
(function() {
"use strict";

function assertAfterClusterTimeReadFailsWithCode(db, readConcernObj, errorCode) {
    return assert.commandFailedWithCode(
        db.runCommand({find: "foo", readConcern: readConcernObj}),
        errorCode,
        "expected command with read concern options: " + tojson(readConcernObj) + " to fail");
}

function assertAfterClusterTimeReadSucceeds(db, readConcernObj) {
    return assert.commandWorked(
        db.runCommand({find: "foo", readConcern: readConcernObj}),
        "expected command with read concern options: " + tojson(readConcernObj) + " to succeed");
}

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        enableMajorityReadConcern: "",
        shardsvr: "",
    }
});

rst.startSet();
rst.initiate();

// Start the sharding test and add the majority read concern enabled replica set.
const st = new ShardingTest({manualAddShard: true});
if (TestData.configShard) {
    assert.commandWorked(st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));
}
assert.commandWorked(st.s.adminCommand({addShard: rst.getURL()}));

const testDB = st.s.getDB("test");

// Insert some data to find later.
assert.commandWorked(
    testDB.runCommand({insert: "foo", documents: [{_id: 1, x: 1}], writeConcern: {w: "majority"}}));

// Test the afterClusterTime API without causal consistency enabled on the mongo connection.

assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "linearizable", afterClusterTime: Timestamp(1, 1)}, ErrorCodes.InvalidOptions);

// Reads with afterClusterTime require a non-zero timestamp.
assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "local", afterClusterTime: {}}, ErrorCodes.TypeMismatch);

assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "local", afterClusterTime: 10}, ErrorCodes.TypeMismatch);

assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "local", afterClusterTime: Timestamp()}, ErrorCodes.InvalidOptions);

assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "local", afterClusterTime: Timestamp(0, 0)}, ErrorCodes.InvalidOptions);

// Reads with proper afterClusterTime arguments return committed data after the given time.
// Reads with afterClusterTime require a non-zero timestamp.
assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "majority", afterClusterTime: {}}, ErrorCodes.TypeMismatch);

assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "majority", afterClusterTime: 10}, ErrorCodes.TypeMismatch);

assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "majority", afterClusterTime: Timestamp()}, ErrorCodes.InvalidOptions);

assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "majority", afterClusterTime: Timestamp(0, 0)}, ErrorCodes.InvalidOptions);

// Reads with proper afterClusterTime arguments return committed data after the given time.
let testReadOwnWrite = function(readConcern) {
    let res = assert.commandWorked(testDB.runCommand(
        {find: "foo", readConcern: {level: readConcern, afterClusterTime: Timestamp(1, 1)}}));

    assert.eq(res.cursor.firstBatch,
              [{_id: 1, x: 1}],
              "expected afterClusterTime read to return the committed document");

    // Test the afterClusterTime API with causal consistency enabled on the mongo connection.
    testDB.getMongo().setCausalConsistency(true);

    // With causal consistency enabled, the shell sets read concern to level "majority" if it is
    // not specified.
    assertAfterClusterTimeReadSucceeds(testDB, {afterClusterTime: Timestamp(1, 1)});
    testDB.getMongo().setCausalConsistency(false);
};

testReadOwnWrite("local");
testReadOwnWrite("majority");

// Read concern levels other than majority are still not accepted.
assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "linearizable", afterClusterTime: Timestamp(1, 1)}, ErrorCodes.InvalidOptions);

// Reads with afterClusterTime still require a non-zero timestamp.
assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "majority", afterClusterTime: {}}, ErrorCodes.TypeMismatch);

assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "majority", afterClusterTime: 10}, ErrorCodes.TypeMismatch);

assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "majority", afterClusterTime: Timestamp()}, ErrorCodes.InvalidOptions);

assertAfterClusterTimeReadFailsWithCode(
    testDB, {level: "majority", afterClusterTime: Timestamp(0, 0)}, ErrorCodes.InvalidOptions);

rst.stopSet();
st.stop();
})();
