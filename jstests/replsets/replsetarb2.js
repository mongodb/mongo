// Election when primary fails and remaining nodes are an arbiter and a secondary.

import {ReplSetTest} from "jstests/libs/replsettest.js";

let replTest = new ReplSetTest({name: "unicomplex", nodes: 3});
let nodes = replTest.nodeList();

let conns = replTest.startSet();
let r = replTest.initiate(
    {
        "_id": "unicomplex",
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1], "arbiterOnly": true, "votes": 1},
            {"_id": 2, "host": nodes[2]},
        ],
    },
    null,
    {initiateWithDefaultElectionTimeout: true},
);

// Make sure we have a primary
let primary = replTest.getPrimary();

// Make sure we have an arbiter
assert.soon(function () {
    let res = conns[1].getDB("admin").runCommand({replSetGetStatus: 1});
    printjson(res);
    return res.myState === 7;
}, "Aribiter failed to initialize.");

let result = conns[1].getDB("admin").runCommand({hello: 1});
assert(result.arbiterOnly);
assert(!result.passive);

// Wait for initial replication
primary.getDB("foo").foo.insert({a: "foo"});
replTest.awaitReplication();

// Now kill the original primary
let pId = replTest.getNodeId(primary);
replTest.stop(pId);

// And make sure that the secondary is promoted
let new_primary = replTest.getPrimary();

let newPrimaryId = replTest.getNodeId(new_primary);
assert.neq(newPrimaryId, pId, "Secondary wasn't promoted to new primary");

replTest.stopSet(15);
