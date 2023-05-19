/**
 * Tests that bsonObjToArray converts BSON objects to JS arrays.
 */

(function() {
'use strict';
const conn = MongoRunner.runMongod();
const db = conn.getDB('test');
const tests = [];

tests.push(function objToArrayOk() {
    assert.eq([1, 2], bsonObjToArray({"a": 1, "b": 2}));
});

tests.push(function sortKeyToArrayOk() {
    assert.commandWorked(db.test.insert({_id: 1, a: 2, b: 2, c: 3}));
    assert.commandWorked(db.test.insert({_id: 2, a: 2, b: 3, c: 4}));
    const findCommand = {
        find: 'test',
        projection: {sortKey: {$meta: 'sortKey'}, _id: 0, a: 0, b: 0, c: 0},
        sort: {a: 1, b: 1},
    };
    const res1 = new DBCommandCursor(db, db.runCommand(findCommand)).toArray();
    assert.eq([2, 2], bsonObjToArray(res1[0]["sortKey"]));
    assert.eq([2, 3], bsonObjToArray(res1[1]["sortKey"]));
});
tests.forEach((test) => {
    jsTest.log(`Starting test '${test.name}'`);
    test();
});

MongoRunner.stopMongod(conn);
})();
