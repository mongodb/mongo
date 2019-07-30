/**
 * Ensures that the 'ns' field for index specs is absent with its removal in SERVER-41696.
 *
 * @tags: [requires_replication, requires_persistence]
 */
(function() {
'use strict';

const dbName = 'test';
const collName = 'absent_ns';

let replSet = new ReplSetTest({name: 'absentNsField', nodes: 2});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);

const secondary = replSet.getSecondary();
const secondaryDB = secondary.getDB(dbName);

assert.commandWorked(primaryColl.insert({x: 100}));
assert.commandWorked(primaryColl.createIndex({x: 1}));

replSet.awaitReplication();

let specPrimary =
    assert.commandWorked(primaryDB.runCommand({listIndexes: collName})).cursor.firstBatch[1];
let specSecondary =
    assert.commandWorked(secondaryDB.runCommand({listIndexes: collName})).cursor.firstBatch[1];

assert.eq(false, specPrimary.hasOwnProperty('ns'));
assert.eq(false, specSecondary.hasOwnProperty('ns'));

replSet.stopSet(/*signal=*/null, /*forRestart=*/true);

// Both nodes should have no 'ns' field in the index spec on restart.
const options = {
    dbpath: primary.dbpath,
    noCleanData: true
};
let conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(options));

let db = conn.getDB(dbName);
let spec = assert.commandWorked(db.runCommand({listIndexes: collName})).cursor.firstBatch[1];

assert.eq(false, spec.hasOwnProperty('ns'));

MongoRunner.stopMongod(conn);

options.dbpath = secondary.dbpath;
conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(options));

db = conn.getDB(dbName);
spec = assert.commandWorked(db.runCommand({listIndexes: collName})).cursor.firstBatch[1];

assert.eq(false, spec.hasOwnProperty('ns'));

MongoRunner.stopMongod(conn);
}());
