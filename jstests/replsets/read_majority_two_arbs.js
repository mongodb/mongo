/**
 * Tests that writeConcern 'majority' writes succeed and are visible in a replica set that has one
 * data-bearing node and two arbiters.
 *
 * @tags: [requires_majority_read_concern]
 */
(function() {
"use strict";

function log(arg) {
    jsTest.log(tojson(arg));
}

// Set up a set and grab things for later.
var name = "read_majority_two_arbs";
var replTest =
    new ReplSetTest({name: name, nodes: 3, nodeOptions: {enableMajorityReadConcern: ''}});

replTest.startSet();
var nodes = replTest.nodeList();
var config = {
    "_id": name,
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1], arbiterOnly: true},
        {"_id": 2, "host": nodes[2], arbiterOnly: true}
    ]
};

replTest.initiate(config);

var primary = replTest.getPrimary();
var db = primary.getDB(name);
var t = db[name];

function doRead(readConcern) {
    readConcern.maxTimeMS = 3000;
    var res = assert.commandWorked(t.runCommand('find', readConcern));
    var docs = (new DBCommandCursor(db, res)).toArray();
    assert.gt(docs.length, 0, "no docs returned!");
    return docs[0].state;
}

function doDirtyRead() {
    log("doing dirty read");
    var ret = doRead({"readConcern": {"level": "local"}});
    log("done doing dirty read.");
    return ret;
}

function doCommittedRead() {
    log("doing committed read");
    var ret = doRead({"readConcern": {"level": "majority"}});
    log("done doing committed read.");
    return ret;
}

jsTest.log("doing write");
assert.commandWorked(
    t.save({_id: 1, state: 0}, {writeConcern: {w: "majority", wtimeout: 10 * 1000}}));
jsTest.log("doing read");
assert.eq(doDirtyRead(), 0);
jsTest.log("doing committed read");
assert.eq(doCommittedRead(), 0);
jsTest.log("stopping replTest; test completed successfully");
replTest.stopSet();
}());
