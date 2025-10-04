/**
 * Test that hyphenated database name can work with dbStats when directoryperdb is turned on.
 *
 * @tags: [requires_persistence]
 */
let isDirectoryPerDBSupported = jsTest.options().storageEngine == "wiredTiger" || !jsTest.options().storageEngine;
if (!isDirectoryPerDBSupported) quit();

const dbName = "test-hyphen";
let conn = MongoRunner.runMongod({directoryperdb: ""});

conn.getDB(dbName).a.insert({x: 1});
let res = conn.getDB(dbName).runCommand({dbStats: 1, scale: 1});
jsTestLog("dbStats: " + tojson(res));
assert(res.db == "test-hyphen");
assert(res.fsUsedSize > 0);
assert(res.fsTotalSize > 0);

MongoRunner.stopMongod(conn);
