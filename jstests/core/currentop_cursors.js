/**
 * Tests that an idle cursor will appear in the $currentOp output if the idleCursors option is
 * set to true.
 *
 * The work to make this feature available on mongos is deferred to SERVER-37004
 * and SERVER-37005. Those tickets will make the idleCursor fields available to curOp.
 * @tags: [assumes_against_mongod_not_mongos, assumes_read_concern_unchanged]
 *
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
                           {$currentOp: {allUsers: false, idleCursors: true}},
                           {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": findOut}]}}
                       ])
                       .toArray();
    assert.eq(result.length, 1, tojson(result));
    assert.eq(result[0].cursor.nDocsReturned, 2, tojson(result));
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
