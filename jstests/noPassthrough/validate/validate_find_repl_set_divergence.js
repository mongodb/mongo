/**
 * Runs extended validate to identify cross-node inconsistencies in different scenarios.
 */

import {runValidateFindDivergenceTests} from "jstests/noPassthrough/validate/libs/validate_find_repl_set_divergence.js";

const dbName = jsTestName();
const collName = jsTestName();
const numDocs = 500;

// Node0
const conn0 = MongoRunner.runMongod();
const db0 = conn0.getDB(dbName);
assert.commandWorked(db0.createCollection(collName));
const coll0 = db0.getCollection(collName);

// Node1
const conn1 = MongoRunner.runMongod();
const db1 = conn1.getDB(dbName);
assert.commandWorked(db1.createCollection(collName));
const coll1 = db1.getCollection(collName);

// Node2
const conn2 = MongoRunner.runMongod();
const db2 = conn2.getDB(dbName);
assert.commandWorked(db2.createCollection(collName));
const coll2 = db2.getCollection(collName);

const colls = [coll0, coll1, coll2];

function getValidateResults(options) {
    let validateResults = [];
    for (let i = 0; i < colls.length; ++i) {
        validateResults.push(colls[i].validate(options));
    }
    return validateResults;
}

runValidateFindDivergenceTests(colls, numDocs, getValidateResults);

MongoRunner.stopMongod(conn0);
MongoRunner.stopMongod(conn1);
MongoRunner.stopMongod(conn2);
