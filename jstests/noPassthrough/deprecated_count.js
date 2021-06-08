// The count command is deprecated in 5.0.
//
// In this test, we run the count command several times.
// We want to make sure that the deprecation warning message is only logged once despite
// the multiple invocations in an effort to not clutter the dev's console.
// More specifically, we expect to only log 1/127 of count() events.

(function() {
"use strict";
load("jstests/libs/log.js");  // For findMatchingLogLine, findMatchingLogLines

jsTest.log('Test standalone');
const standalone = MongoRunner.runMongod({});
const dbName = 'test';
const collName = "test_count_command_deprecation_messaging";
const db = standalone.getDB(dbName);
const coll = db.getCollection(collName);

coll.drop();
var res = assert.commandWorked(db.runCommand({count: collName}));
assert.eq(0, res.n);
assert.commandWorked(coll.insert({i: 1}));
res = assert.commandWorked(db.runCommand({count: collName}));
assert.eq(1, res.n);
assert.commandWorked(coll.insert({i: 1}));
res = assert.commandWorked(db.runCommand({count: collName}));
assert.eq(2, res.n);

const globalLogs = db.adminCommand({getLog: 'global'});
const fieldMatcher = {
    msg:
        "The count command is deprecated. For more information, see https://docs.mongodb.com/manual/reference/method/db.collection.count/"
};
const matchingLogLines = [...findMatchingLogLines(globalLogs.log, fieldMatcher)];
assert.eq(matchingLogLines.length, 1, matchingLogLines);
MongoRunner.stopMongod(standalone);
})();
