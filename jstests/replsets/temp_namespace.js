// SERVER-10927
// This is to make sure that temp collections get cleaned up on promotion to primary
// @tags: [requires_replication]

import {ReplSetTest} from "jstests/libs/replsettest.js";

let replTest = new ReplSetTest({name: "testSet", nodes: 3});
let nodes = replTest.nodeList();
printjson(nodes);

// We need an arbiter to ensure that the primary doesn't step down when we restart the secondary
replTest.startSet();
replTest.initiate(
    {
        "_id": "testSet",
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2], "arbiterOnly": true},
        ],
    },
    null,
    {initiateWithDefaultElectionTimeout: true},
);

let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

let primaryDB = primary.getDB("test");
let secondaryDB = secondary.getDB("test");

// set up collections
assert.commandWorked(
    primaryDB.runCommand({applyOps: [{op: "c", ns: primaryDB.getName() + ".$cmd", o: {create: "temp1", temp: true}}]}),
);
primaryDB.temp1.createIndex({x: 1});
assert.commandWorked(
    primaryDB.runCommand({applyOps: [{op: "c", ns: primaryDB.getName() + ".$cmd", o: {create: "temp2", temp: 1}}]}),
);
primaryDB.temp2.createIndex({x: 1});
assert.commandWorked(
    primaryDB.runCommand({applyOps: [{op: "c", ns: primaryDB.getName() + ".$cmd", o: {create: "keep1", temp: false}}]}),
);
assert.commandWorked(
    primaryDB.runCommand({applyOps: [{op: "c", ns: primaryDB.getName() + ".$cmd", o: {create: "keep2", temp: 0}}]}),
);
primaryDB.runCommand({create: "keep3"});
assert.commandWorked(primaryDB.keep4.insert({}, {writeConcern: {w: 2}}));

// make sure they exist on primary and secondary
function countCollection(mydb, nameFilter) {
    let result = mydb.runCommand("listCollections", {filter: {name: nameFilter}});
    assert.commandWorked(result);
    return new DBCommandCursor(mydb, result).itcount();
}

function countIndexesFor(mydb, nameFilter) {
    let result = mydb.runCommand("listCollections", {filter: {name: nameFilter}});
    assert.commandWorked(result);
    let arr = new DBCommandCursor(mydb, result).toArray();
    let total = 0;
    for (let i = 0; i < arr.length; i++) {
        let coll = arr[i];
        total += mydb.getCollection(coll.name).getIndexes().length;
    }
    return total;
}

assert.eq(countCollection(primaryDB, /temp\d$/), 2); // collections
assert.eq(countIndexesFor(primaryDB, /temp\d$/), 4); // indexes (2 _id + 2 x)
assert.eq(countCollection(primaryDB, /keep\d$/), 4);

assert.eq(countCollection(secondaryDB, /temp\d$/), 2); // collections
assert.eq(countIndexesFor(secondaryDB, /temp\d$/), 4); // indexes (2 _id + 2 x)
assert.eq(countCollection(secondaryDB, /keep\d$/), 4);

// restart secondary and reconnect
replTest.restart(replTest.getNodeId(secondary), {}, /*wait=*/ true);

// wait for the secondary to achieve secondary status
assert.soon(
    function () {
        try {
            let res = secondary.getDB("admin").runCommand({replSetGetStatus: 1});
            return res.myState == 2;
        } catch (e) {
            return false;
        }
    },
    "took more than a minute for the secondary to become secondary again",
    60 * 1000,
);

// make sure restarting secondary didn't drop collections
assert.eq(countCollection(secondaryDB, /temp\d$/), 2); // collections
assert.eq(countIndexesFor(secondaryDB, /temp\d$/), 4); // indexes (2 _id + 2 x)
assert.eq(countCollection(secondaryDB, /keep\d$/), 4);

// step down primary and make sure former secondary (now primary) drops collections
assert.commandWorked(primary.adminCommand({replSetStepDown: 50, force: true}));

assert.soon(
    function () {
        return secondaryDB.hello().isWritablePrimary;
    },
    "",
    75 * 1000,
); // must wait for secondary to be willing to promote self

assert.eq(countCollection(secondaryDB, /temp\d$/), 0); // collections
assert.eq(countIndexesFor(secondaryDB, /temp\d$/), 0); // indexes (2 _id + 2 x)
assert.eq(countCollection(secondaryDB, /keep\d$/), 4);

// check that former primary dropped collections
replTest.awaitReplication();
assert.eq(countCollection(primaryDB, /temp\d$/), 0); // collections
assert.eq(countIndexesFor(primaryDB, /temp\d$/), 0); // indexes (2 _id + 2 x)
assert.eq(countCollection(primaryDB, /keep\d$/), 4);

replTest.stopSet();
