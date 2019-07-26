/**
 * Uses the 'disableIndexSpecNamespaceGeneration' server test parameter to disable the generation
 * of the 'ns' field for index specs to test the absence of the field.
 *
 * When the 'ns' field is missing from the index specs and the 'disableIndexSpecNamespaceGeneration'
 * server test parameter is disabled, the server should automatically add the 'ns' field to the
 * index specs missing it prior to returning them.
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

// The primary will not generate the 'ns' field for index specs, but the secondary will.
assert.commandWorked(
    primary.getDB('admin').runCommand({setParameter: 1, disableIndexSpecNamespaceGeneration: 1}));

assert.commandWorked(primaryColl.insert({x: 100}));
assert.commandWorked(primaryColl.createIndex({x: 1}));

replSet.awaitReplication();

let specPrimary =
    assert.commandWorked(primaryDB.runCommand({listIndexes: collName})).cursor.firstBatch[1];
let specSecondary =
    assert.commandWorked(secondaryDB.runCommand({listIndexes: collName})).cursor.firstBatch[1];

assert.eq(false, specPrimary.hasOwnProperty('ns'));
assert.eq(true, specSecondary.hasOwnProperty('ns'));
assert.eq(dbName + '.' + collName, specSecondary.ns);

replSet.stopSet(/*signal=*/null, /*forRestart=*/true);

// The primaries index spec has no 'ns' field and the secondaries index spec does have the 'ns'
// field. Restart the nodes as standalone and ensure that the primaries index spec gets updated
// with the 'ns' field. No changes should be necessary to the secondaries index spec, but
// verify that it still has the 'ns' field.
const options = {
    dbpath: primary.dbpath,
    noCleanData: true
};
let conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(options));

let db = conn.getDB(dbName);
let spec = assert.commandWorked(db.runCommand({listIndexes: collName})).cursor.firstBatch[1];

assert.eq(true, spec.hasOwnProperty('ns'));
assert.eq(dbName + '.' + collName, spec.ns);

MongoRunner.stopMongod(conn);

options.dbpath = secondary.dbpath;
conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(options));

db = conn.getDB(dbName);
spec = assert.commandWorked(db.runCommand({listIndexes: collName})).cursor.firstBatch[1];

assert.eq(true, spec.hasOwnProperty('ns'));
assert.eq(dbName + '.' + collName, spec.ns);

MongoRunner.stopMongod(conn);
}());
