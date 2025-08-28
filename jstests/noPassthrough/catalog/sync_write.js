/**
 * SERVER-20617: Tests that journaled write operations survive a kill -9 of the mongod.
 *
 * This test requires persistence to ensure data survives a restart.
 * @tags: [requires_persistence]
 */
//  The following test verifies that writeConcern: {j: true} ensures that data is durable.
let dbpath = MongoRunner.dataPath + jsTestName();
resetDbpath(dbpath);

let mongodArgs = {dbpath: dbpath, noCleanData: true};

// Start a mongod.
let conn = MongoRunner.runMongod(mongodArgs);
assert.neq(null, conn, "mongod was unable to start up");

// Now connect to the mongod, do a journaled write and abruptly stop the server.
let testDB = conn.getDB("test");
assert.commandWorked(testDB.synced.insert({synced: true}, {writeConcern: {j: true}}));
MongoRunner.stopMongod(conn, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});

// Restart the mongod.
conn = MongoRunner.runMongod(mongodArgs);
assert.neq(null, conn, "mongod was unable to restart after receiving a SIGKILL");

// Check that our journaled write still is present.
testDB = conn.getDB("test");
assert.eq(1, testDB.synced.count({synced: true}), "synced write was not found");
MongoRunner.stopMongod(conn);
