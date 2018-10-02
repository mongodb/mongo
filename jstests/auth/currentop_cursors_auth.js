/**
 * Tests whether cursors will show up if the caller of currentOp is not authorized.
 * @tags: [assumes_read_concern_unchanged, requires_auth]
 */

(function() {
    "use strict";
    const m = MongoRunner.runMongod({auth: ""});
    const db = m.getDB("test");
    const adminDB = db.getSiblingDB("admin");
    Random.setRandomSeed();
    const pass = "a" + Random.rand();
    adminDB.createUser({user: "ted", pwd: pass, roles: ["root"]});
    assert(adminDB.auth("ted", pass), "Authentication 1 Failed");
    adminDB.createUser({user: "yuta", pwd: pass, roles: ["readWriteAnyDatabase"]});

    const coll = db.jstests_currentop;
    coll.drop();
    for (let i = 0; i < 5; ++i) {
        assert.commandWorked(coll.insert({val: i}));
    }
    const cursorId =
        assert.commandWorked(db.runCommand({find: "jstests_currentop", batchSize: 2})).cursor.id;

    let result = adminDB
                     .aggregate([
                         {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                         {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": cursorId}]}}
                     ])
                     .toArray();
    // This is our cursor, so it should appear.
    assert.eq(result.length, 1, result);
    assert.commandWorked(adminDB.logout());
    assert(adminDB.auth("yuta", pass), "Authentication 2 Failed");

    result = adminDB
                 .aggregate([
                     {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                     {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": cursorId}]}}
                 ])
                 .toArray();
    // Changed user, so cursor should not appear.
    assert.eq(result.length, 0, result);

    // Make sure behavior is the same with allUsers not specified.
    result = adminDB
                 .aggregate([
                     {$currentOp: {localOps: true, idleCursors: true}},
                     {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": cursorId}]}}
                 ])
                 .toArray();
    assert.eq(result.length, 0, result);

    // Create a cursor with the second user.

    const secondCursorId =
        assert.commandWorked(db.runCommand({find: "jstests_currentop", batchSize: 2})).cursor.id;

    assert.commandWorked(adminDB.logout());
    // Make sure cursor is still there to double check it was not seen because of auth.
    assert(adminDB.auth("ted", pass), "Authentication 3 Failed");

    result = adminDB
                 .aggregate([
                     {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                     {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": cursorId}]}}
                 ])
                 .toArray();
    assert.eq(result.length, 1, result);

    // Make sure with allUsers set to true the root user can see both cursors.
    result = adminDB
                 .aggregate([
                     {$currentOp: {localOps: true, allUsers: true, idleCursors: true}},
                     {
                       $match: {
                           $and: [
                               {type: "idleCursor"},
                               {
                                 $or: [
                                     {"cursor.cursorId": secondCursorId},
                                     {"cursor.cursorId": cursorId}
                                 ]
                               }
                           ]
                       }
                     }
                 ])
                 .toArray();
    assert.eq(result.length, 2, result);

    MongoRunner.stopMongod(m, null, {user: "ted", pwd: pass});
})();
