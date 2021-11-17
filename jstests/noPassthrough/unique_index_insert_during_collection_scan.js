/**
 * Tests inserting into collection while a unique index build is in the collection scan phase.
 * Ensures that even though the insert is seen by both the collection scan and the side writes
 * table, the index build does not need to resolve any duplicate keys.
 */
(function() {
"use strict";

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');

const conn = MongoRunner.runMongod();
const coll = conn.getDB('test')[jsTestName()];

assert.commandWorked(coll.insert({_id: 0, a: 0}));

const fp = configureFailPoint(conn, 'hangAfterInitializingIndexBuild');
const awaitCreateIndex =
    startParallelShell(funWithArgs(function(collName) {
                           assert.commandWorked(db[collName].createIndex({a: 1}, {unique: true}));
                       }, coll.getName()), conn.port);
fp.wait();
assert.commandWorked(coll.insert({_id: 1, a: 1}));
fp.off();

awaitCreateIndex();

// Ensure that the second document was seen by both the collection scan and the side writes table.
checkLog.containsJson(conn, 20685, {namespace: coll.getFullName(), index: 'a_1', keysInserted: 2});
checkLog.containsJson(
    conn, 20689, {namespace: coll.getFullName(), index: 'a_1', numApplied: 1, totalInserted: 1});

// Ensure that there were no duplicates to resolve.
assert(!checkLog.checkContainsOnceJson(conn, 20677, {indexName: 'a_1'}));

MongoRunner.stopMongod(conn);
})();
