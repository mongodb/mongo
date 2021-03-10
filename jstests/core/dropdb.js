/**
 * Test that a db does not exist after it is dropped.
 *
 * @tags: [
 *   # listDatabases with explicit filter on db names doesn't work on tenant migrations suites
 *   tenant_migration_incompatible,
 *   ]
 */

(function() {
"use strict";

function listDatabases(options) {
    return assert
        .commandWorked(db.adminCommand(Object.assign({listDatabases: 1, nameOnly: true}, options)))
        .databases;
}

function assertDatabaseDoesNotExist(dbName) {
    assert.eq(0, listDatabases({filter: {name: dbName}}).length);
}

function assertDatabaseExists(dbName) {
    assert.eq(1,
              listDatabases({filter: {name: dbName}}).length,
              "database " + dbName + " not found in " + tojson(listDatabases()));
}

let ddb = db.getSiblingDB("jstests_dropdb");

jsTest.log("Initial DBs: " + tojson(listDatabases()));

ddb.c.save({});
assertDatabaseExists(ddb.getName());

assert.commandWorked(ddb.dropDatabase());
assertDatabaseDoesNotExist(ddb.getName());

assert.commandWorked(ddb.dropDatabase());
assertDatabaseDoesNotExist(ddb.getName());
})();
