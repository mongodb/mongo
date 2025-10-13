/**
 * Runs extended validate to identify cross-node inconsistencies in different scenarios.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */

import {reconnect} from "jstests/replsets/rslib.js";
import {runValidateFindDivergenceTests} from "jstests/noPassthrough/validate/libs/validate_find_repl_set_divergence.js";

const dbpath0 = MongoRunner.dataPath + jsTestName() + "_0";
const dbpath1 = MongoRunner.dataPath + jsTestName() + "_1";
const dbpath2 = MongoRunner.dataPath + jsTestName() + "_2";
const dbName = jsTestName();
const collName = jsTestName();
const numDocs = 500;

// Node0
let conn0 = MongoRunner.runMongod({dbpath: dbpath0});
const port0 = conn0.port;
const db0 = conn0.getDB(dbName);
assert.commandWorked(db0.createCollection(collName));
const coll0 = db0.getCollection(collName);

// Node1
let conn1 = MongoRunner.runMongod({dbpath: dbpath1});
const port1 = conn1.port;
const db1 = conn1.getDB(dbName);
assert.commandWorked(db1.createCollection(collName));
const coll1 = db1.getCollection(collName);

// Node2
let conn2 = MongoRunner.runMongod({dbpath: dbpath2});
const port2 = conn2.port;
const db2 = conn2.getDB(dbName);
assert.commandWorked(db2.createCollection(collName));
const coll2 = db2.getCollection(collName);

const colls = [coll0, coll1, coll2];
let conns = [conn0, conn1, conn2];
const dbpaths = [dbpath0, dbpath1, dbpath2];
const ports = [port0, port1, port2];

function runValidate(dbpath, port, opts) {
    jsTest.log.info(`Running validate with ${tojson(opts)}`);
    MongoRunner.runMongod({
        dbpath: dbpath,
        port: port,
        validate: "",
        setParameter: {
            validateDbName: dbName,
            validateCollectionName: collName,
            collectionValidateOptions: {options: opts},
        },
        noCleanData: true,
    });
    const validateResults = rawMongoProgramOutput("(9437301)")
        .split("\n")
        .filter((line) => line.trim() !== "")
        .map((line) => JSON.parse(line.split("|").slice(1).join("|")));
    assert.eq(validateResults.length, 1);
    jsTest.log.info(`Validate result with ${tojson(opts)}:\n${tojson(validateResults[0])}`);
    clearRawMongoProgramOutput();
    return validateResults[0].attr.results;
}

function getValidateResults(options) {
    let validateResults = [];
    for (let i = 0; i < conns.length; ++i) {
        MongoRunner.stopMongod(conns[i], null, {skipValidation: true});
        validateResults.push(runValidate(dbpaths[i], ports[i], options));
        MongoRunner.runMongod({dbpath: dbpaths[i], port: ports[i], noCleanData: true});
        reconnect(conns[i]);
    }
    return validateResults;
}

runValidateFindDivergenceTests(colls, numDocs, getValidateResults);

MongoRunner.stopMongod(conn0);
MongoRunner.stopMongod(conn1);
MongoRunner.stopMongod(conn2);
