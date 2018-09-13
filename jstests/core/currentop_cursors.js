/**
 * Tests that an idle cursor will appear in the $currentOp output if the idleCursors option is
 * set to true.
 *
 * @tags: [assumes_read_concern_unchanged]
 */

(function() {
    "use strict";
    const coll = db.jstests_currentop;
    const adminDB = db.getSiblingDB("admin");
    coll.drop();
    for (let i = 0; i < 5; ++i) {
        assert.commandWorked(coll.insert({"val": i}));
    }
    const findOut =
        assert.commandWorked(db.runCommand({find: "jstests_currentop", batchSize: 2})).cursor.id;
    const result = adminDB
                       .aggregate([
                           {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                           {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": findOut}]}}
                       ])
                       .toArray();
    assert.eq(result.length, 1, result);
    assert.eq(result[0].cursor.nDocsReturned, 2, result);
    assert.eq(result[0].cursor.tailable, false, result);
    assert.eq(result[0].cursor.awaitData, false, result);
    assert.eq(result[0].cursor.noCursorTimeout, false, result);
    assert.eq(result[0].cursor.ns, coll.getFullName(), result);
    assert.eq(result[0].cursor.originatingCommand.find, "jstests_currentop", result);
    assert.eq(result[0].cursor.originatingCommand.batchSize, 2, result);

    const noIdle = adminDB
                       .aggregate([
                           {$currentOp: {allUsers: false, idleCursors: false}},
                           {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": findOut}]}}
                       ])
                       .toArray();
    assert.eq(noIdle.length, 0, tojson(noIdle));
    const noFlag =
        adminDB.aggregate([{$currentOp: {allUsers: false}}, {$match: {type: "idleCursor"}}])
            .toArray();

    assert.eq(noIdle.length, 0, tojson(noFlag));
})();
