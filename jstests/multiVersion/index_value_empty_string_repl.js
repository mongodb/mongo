/*
 * Test that a repl set with a 4.0 version primary and latest secondary will allow replication of
 * index key values of empty strings.
 */

(function() {
"use strict";
load('./jstests/multiVersion/libs/multi_rs.js');

const newVersion = "latest";
const oldVersion = "last-stable";

const name = "index_value_empty_string_repl";
let nodes = {
    n1: {binVersion: oldVersion},
    n2: {binVersion: newVersion, rsConfig: {priority: 0}},
};

const rst = new ReplSetTest({name: name, nodes: nodes, waitForKeys: true});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');

assert.commandWorked(testDB.testColl.createIndex({x: ""}));
rst.awaitReplication();

rst.add({binVersion: newVersion, rsConfig: {priority: 0}});
rst.reInitiate();

rst.awaitSecondaryNodes();
rst.awaitReplication();
rst.stopSet();
})();
