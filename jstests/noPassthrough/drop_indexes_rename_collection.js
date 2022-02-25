/**
 * Tests the dropIndexes command while the collection is concurrently renamed.
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');

const conn = MongoRunner.runMongod();

const db = conn.getDB(jsTestName());
const coll = db.coll;

assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.createIndex({a: 1}));

const fp = configureFailPoint(conn, 'hangAfterAbortingIndexes');

const awaitDropIndexes = startParallelShell(
    funWithArgs(function(dbName, collName) {
        assert.commandWorked(db.getSiblingDB(dbName)[collName].dropIndex({a: 1}));
    }, db.getName(), coll.getName()), conn.port);

fp.wait();
assert.commandWorked(coll.renameCollection('coll_2'));
fp.off();

awaitDropIndexes();

MongoRunner.stopMongod(conn);
})();
