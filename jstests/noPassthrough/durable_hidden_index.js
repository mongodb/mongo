/**
 * Test that hidden index status can be replicated by secondary nodes and will be persisted
 * into the index catalog, that is hidden index remains hidden after restart.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/index_catalog_helpers.js");  // For IndexCatalogHelpers.findByName.

const dbName = "test";

function isIndexHidden(indexes, indexName) {
    const idx = IndexCatalogHelpers.findByName(indexes, indexName);
    return idx && idx.hidden;
}

//
// Test that hidden index status can be replicated by secondary nodes and will be persisted into the
// index catalog in a replica set.
//
const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const primaryDB = rst.getPrimary().getDB(dbName);
primaryDB.coll.drop();

// Create a hidden index.
assert.commandWorked(primaryDB.coll.createIndex({a: 1}, {hidden: true}));
assert(isIndexHidden(primaryDB.coll.getIndexes(), "a_1"));

// Explicitly create an unhidden index.
assert.commandWorked(primaryDB.coll.createIndex({b: 1}, {hidden: false}));
assert(!isIndexHidden(primaryDB.coll.getIndexes(), "b_1"));

// Wait for the replication finishes before stopping the replica set.
rst.awaitReplication();

// Restart the replica set.
rst.stopSet(/* signal */ undefined, /* forRestart */ true);
rst.startSet(/* signal */ undefined, /* forRestart */ true);
const secondaryDB = rst.getSecondary().getDB(dbName);

// Test that after restart the index is still hidden.
assert(isIndexHidden(secondaryDB.coll.getIndexes(), "a_1"));

// Test that 'hidden: false' shouldn't be written to the index catalog.
let idxSpec = IndexCatalogHelpers.findByName(secondaryDB.coll.getIndexes(), "b_1");
assert.eq(idxSpec.hidden, undefined);

rst.stopSet();

//
// Test that hidden index status will be persisted into the index catalog in a standalone mongod,
// whereas, an unhidden index will not write 'hidden: false' to the index catalog even when
// createIndexes specifies 'hidden: false' explicitly.
//
// Start a mongod.
let conn = MongoRunner.runMongod();
assert.neq(null, conn, 'mongod was unable to start up');
let db = conn.getDB(dbName);
db.coll.drop();

// Create a hidden index.
assert.commandWorked(db.coll.createIndex({a: 1}, {hidden: true}));
assert(isIndexHidden(db.coll.getIndexes(), "a_1"));

// Explicitly create an unhidden index.
assert.commandWorked(db.coll.createIndex({b: 1}, {hidden: false}));
assert(!isIndexHidden(db.coll.getIndexes(), "b_1"));

// Restart the mongod.
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod({restart: true, cleanData: false, dbpath: conn.dbpath});
db = conn.getDB(dbName);

// Test that after restart the index is still hidden.
assert(isIndexHidden(db.coll.getIndexes(), "a_1"));

// Test that 'hidden: false' shouldn't be written to the index catalog.
idxSpec = IndexCatalogHelpers.findByName(db.coll.getIndexes(), "b_1");
assert.eq(idxSpec.hidden, undefined);

MongoRunner.stopMongod(conn);
})();
