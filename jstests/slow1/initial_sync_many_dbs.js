/**
 * Runs initial sync on a node with many databases.
 *
 * @tags: [
 *   # We are choosing not to test on MacOS since it's too noisy.
 *   slow_on_macos
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "initial_sync_many_dbs";
let num_dbs = 32;
let max_colls = 32;
let num_docs = 2;
let replSet = new ReplSetTest({
    name: name,
    nodes: 1,
});
replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();
jsTestLog(
    "Seeding primary with " +
        num_dbs +
        " databases with up to " +
        max_colls +
        " collections each. Each collection will contain " +
        num_docs +
        " documents",
);
for (let i = 0; i < num_dbs; i++) {
    let dbname = name + "_db" + i;
    for (let j = 0; j < (i % max_colls) + 1; j++) {
        let collname = name + "_coll" + j;
        let coll = primary.getDB(dbname)[collname];
        for (let k = 0; k < num_docs; k++) {
            assert.commandWorked(coll.insert({_id: k}));
        }
    }
}

// Add a secondary that will initial sync from the primary.
jsTestLog("Adding node to replica set to trigger initial sync process");
replSet.add();
replSet.reInitiate();

replSet.awaitSecondaryNodes(30 * 60 * 1000);
let secondary = replSet.getSecondary();
jsTestLog("New node has transitioned to secondary. Checking collection sizes");
for (let i = 0; i < num_dbs; i++) {
    let dbname = name + "_db" + i;
    for (let j = 0; j < (i % max_colls) + 1; j++) {
        let collname = name + "_coll" + j;
        let coll = secondary.getDB(dbname)[collname];
        assert.eq(
            num_docs,
            coll.find().itcount(),
            "collection size inconsistent with primary after initial sync: " + coll.getFullName(),
        );
    }
}

replSet.stopSet();
