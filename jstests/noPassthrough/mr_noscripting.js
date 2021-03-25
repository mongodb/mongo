// Tests that running mapReduce does not crash mongod if scripting is disabled.
(function() {
"use strict";

const conn = MongoRunner.runMongod({noscripting: ''});
const testDB = conn.getDB('foo');
const coll = testDB.bar;

assert.commandWorked(coll.insert({x: 1}));

const map = function() {
    emit(this.x, 1);
};

const reduce = function(key, values) {
    return 1;
};

assert.commandFailedWithCode(
    testDB.runCommand({mapReduce: 'bar', map: map, reduce: reduce, out: {inline: 1}}), 31264);

MongoRunner.stopMongod(conn);
}());
