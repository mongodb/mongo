/**
 * Test that the server doesn't warn about clustered collections missing the index on _id
 *
 * We restart mongod during the test and expect it to have the same data after restarting.
 * @tags: [requires_persistence, requires_fcv_52, requires_replication]
 */

(function() {
"use strict";

const testName = "clustered_collection_at_startup";
const dbpath = MongoRunner.dataPath + testName;
const coll = "clusteredCollection";

const replSet = new ReplSetTest({name: testName, nodes: 1, nodeOptions: {dbpath: dbpath}});
replSet.startSet();
replSet.initiate();

// Create clustered collection.
{
    const conn = replSet.getPrimary();
    assert.neq(null, conn, "mongod was unable to start up");

    const testDB = conn.getDB("test");
    assert.commandWorked(
        testDB.createCollection(coll, {clusteredIndex: {key: {_id: 1}, unique: true}}));
    assert.commandWorked(testDB[coll].insert({_id: 0, a: 2}));

    replSet.restart(conn);
}

// Check there's no warning after restart.
{
    const conn = replSet.getPrimary();
    assert.neq(null, conn, "mongod was unable to start up");

    const testDB = conn.getDB("test");

    assert.eq(1, testDB[coll].find({}).itcount());

    // Check that we didn't log a startup warning.
    const cmdRes = assert.commandWorked(testDB.adminCommand({getLog: "startupWarnings"}));
    assert(!/20322/.test(cmdRes.log));
}

replSet.stopSet();
})();
