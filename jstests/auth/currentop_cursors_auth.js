/**
 * Tests that a user's ability to view open cursors via $currentOp obeys authentication rules on
 * both mongoD and mongoS.
 * @tags: [assumes_read_concern_unchanged, requires_auth, requires_replication]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isMongos.

// Create a new sharded cluster for testing and enable auth.
const key = "jstests/libs/key1";
const st = new ShardingTest({name: jsTestName(), keyFile: key, shards: 1});

const shardConn = st.rs0.getPrimary();
const mongosConn = st.s;

shardConn.waitForClusterTime(60);

Random.setRandomSeed();
const pass = "a" + Random.rand();

// Create one root user and one regular user on the given connection.
function createUsers(conn) {
    const adminDB = conn.getDB("admin");
    adminDB.createUser({user: "ted", pwd: pass, roles: ["root"]});
    assert(adminDB.auth("ted", pass), "Authentication 1 Failed");
    adminDB.createUser({user: "yuta", pwd: pass, roles: ["readWriteAnyDatabase"]});
}

// Create the necessary users at both cluster and shard-local level.
createUsers(shardConn);
createUsers(mongosConn);

// Run the various auth tests on the given shard or mongoS connection.
function runCursorTests(conn) {
    const db = conn.getDB("test");
    const adminDB = db.getSiblingDB("admin");

    // Log in as the root user.
    assert.commandWorked(adminDB.logout());
    assert(adminDB.auth("ted", pass), "Authentication 2 Failed");

    const coll = db.jstests_currentop_cursors_auth;
    coll.drop();
    for (let i = 0; i < 5; ++i) {
        assert.commandWorked(coll.insert({val: i}));
    }

    // Verify that we can see our own cursor with {allUsers: false}.
    const cursorId =
        assert.commandWorked(db.runCommand({find: "jstests_currentop_cursors_auth", batchSize: 2}))
            .cursor.id;

    let result = adminDB
                     .aggregate([
                         {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                         {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": cursorId}]}}
                     ])
                     .toArray();
    assert.eq(result.length, 1, result);

    // Log in as the non-root user.
    assert.commandWorked(adminDB.logout());
    assert(adminDB.auth("yuta", pass), "Authentication 3 Failed");

    // Verify that we cannot see the root user's cursor.
    result = adminDB
                 .aggregate([
                     {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                     {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": cursorId}]}}
                 ])
                 .toArray();
    assert.eq(result.length, 0, result);

    // Make sure that the behavior is the same when 'allUsers' is not explicitly specified.
    result = adminDB
                 .aggregate([
                     {$currentOp: {localOps: true, idleCursors: true}},
                     {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": cursorId}]}}
                 ])
                 .toArray();
    assert.eq(result.length, 0, result);

    // Verify that the user without the 'inprog' privilege cannot view shard cursors via mongoS.
    if (FixtureHelpers.isMongos(db)) {
        assert.commandFailedWithCode(adminDB.runCommand({
            aggregate: 1,
            pipeline: [{$currentOp: {localOps: false, idleCursors: true}}],
            cursor: {}
        }),
                                     ErrorCodes.Unauthorized);
    }

    // Create a cursor with the second (non-root) user and confirm that we can see it.
    const secondCursorId =
        assert.commandWorked(db.runCommand({find: "jstests_currentop_cursors_auth", batchSize: 2}))
            .cursor.id;

    result = adminDB
                 .aggregate([
                     {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                     {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": secondCursorId}]}}
                 ])
                 .toArray();
    assert.eq(result.length, 1, result);

    // Log back in with the root user and confirm that the first cursor is still present.
    assert.commandWorked(adminDB.logout());
    assert(adminDB.auth("ted", pass), "Authentication 4 Failed");

    result = adminDB
                 .aggregate([
                     {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                     {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": cursorId}]}}
                 ])
                 .toArray();
    assert.eq(result.length, 1, result);

    // Confirm that the root user can see both users' cursors with {allUsers: true}.
    result =
        adminDB
            .aggregate([
                {$currentOp: {localOps: true, allUsers: true, idleCursors: true}},
                {$match: {type: "idleCursor", "cursor.cursorId": {$in: [cursorId, secondCursorId]}}}
            ])
            .toArray();
    assert.eq(result.length, 2, result);

    // The root user can also see both cursors on the shard via mongoS with {localOps: false}.
    if (FixtureHelpers.isMongos(db)) {
        result = adminDB
                     .aggregate([
                         {$currentOp: {localOps: false, allUsers: true, idleCursors: true}},
                         {$match: {type: "idleCursor", shard: st.rs0.name}}
                     ])
                     .toArray();
        assert.eq(result.length, 2, result);
    }

    // Clean up the cursors so that they don't affect subsequent tests.
    assert.commandWorked(
        db.runCommand({killCursors: coll.getName(), cursors: [cursorId, secondCursorId]}));

    // Make sure to logout to allow __system user to use the implicit session.
    assert.commandWorked(adminDB.logout());
}

jsTestLog("Running cursor tests on mongoD");
runCursorTests(shardConn);

jsTestLog("Running cursor tests on mongoS");
runCursorTests(mongosConn);

st.stop();
})();
