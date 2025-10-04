/**
 * Tests that the validate command detects incorrect WT table logging settings.
 *
 * @tags: [
 *   requires_wiredtiger,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let conn = MongoRunner.runMongod();

const dbpath = conn.dbpath;
const dbName = jsTestName();
const collName = "coll";

const create = function (conn) {
    assert.commandWorked(conn.getDB(dbName).createCollection(collName));
};

const collUri = function (conn) {
    return conn
        .getDB(dbName)
        [collName].aggregate([{$collStats: {storageStats: {}}}])
        .toArray()[0]
        .storageStats.wiredTiger.uri.split("statistics:")[1];
};

const indexUri = function (conn, indexName) {
    return conn
        .getDB(dbName)
        [collName].aggregate([{$collStats: {storageStats: {}}}])
        .toArray()[0]
        .storageStats.indexDetails[indexName].uri.split("statistics:")[1];
};

// Create the collection and indexes as a standlone, which will cause the tables to be logged.
create(conn);
MongoRunner.stopMongod(conn);

const nodeOptions = {
    dbpath: dbpath,
    noCleanData: true,
    // Skip the normal step of switching the logging setting on the tables.
    setParameter: {wiredTigerSkipTableLoggingChecksOnStartup: true},
};
const replTest = new ReplSetTest({
    nodes: 1,
    nodeOptions: nodeOptions,
});
replTest.startSet();
replTest.initiate();
const primary = replTest.getPrimary();

// Run validate as a replica set, which will expect the tables to not be logged.
let res = assert.commandWorked(primary.getDB(dbName).runCommand({validate: collName}));
assert(!res.valid);
assert.eq(res.errors.length, 2);
checkLog.containsJson(primary, 6898101, {uri: collUri(primary), expected: false});
checkLog.containsJson(primary, 6898101, {index: "_id_", uri: indexUri(primary, "_id_"), expected: false});

// Create the collection and indexes as a replica set, which will cause the tables to not be logged.
assert.commandWorked(primary.getDB(dbName).runCommand({drop: collName}));
create(primary);

replTest.stopSet(null, false, {noCleanData: true, skipValidation: true});
conn = MongoRunner.runMongod(nodeOptions);

// Run validate as a standalone, which will expect the tables to be logged.
res = assert.commandWorked(conn.getDB(dbName).runCommand({validate: collName}));
assert(!res.valid);
assert.eq(res.errors.length, 2);
checkLog.containsJson(conn, 6898101, {uri: collUri(conn), expected: true});
checkLog.containsJson(conn, 6898101, {index: "_id_", uri: indexUri(conn, "_id_"), expected: true});

MongoRunner.stopMongod(conn, null, {skipValidation: true});
