import {ReplSetTest} from "jstests/libs/replsettest.js";
import {syncFrom} from "jstests/replsets/rslib.js";

function myprint(x) {
    print("chaining output: " + x);
}

let replTest = new ReplSetTest({name: "testSet", nodes: 3, useBridge: true});
let nodes = replTest.startSet();
let hostnames = replTest.nodeList();
replTest.initiate({
    "_id": "testSet",
    "members": [
        {"_id": 0, "host": hostnames[0], priority: 2},
        {"_id": 1, "host": hostnames[1], priority: 0},
        {"_id": 2, "host": hostnames[2], priority: 0},
    ],
    "settings": {"chainingAllowed": false},
});

let primary = replTest.getPrimary();
// The default WC is majority and stopServerReplication could prevent satisfying any majority
// writes.
assert.commandWorked(
    primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
);

replTest.awaitReplication();

let breakNetwork = function () {
    nodes[0].disconnect(nodes[2]);
    primary = replTest.getPrimary();
};

let checkNoChaining = function () {
    primary.getDB("test").foo.insert({x: 1});

    assert.soon(function () {
        return nodes[1].getDB("test").foo.findOne() != null;
    });

    let endTime = new Date().getTime() + 10000;
    while (new Date().getTime() < endTime) {
        assert(nodes[2].getDB("test").foo.findOne() == null, "Check that 2 does not catch up");
    }
};

let forceSync = function () {
    syncFrom(nodes[2], nodes[1], replTest);
    assert.soon(function () {
        return nodes[2].getDB("test").foo.findOne() != null;
    }, "Check for data after force sync");
};

// SERVER-12922
//
if (!_isWindows()) {
    print("break the network so that node 2 cannot replicate");
    breakNetwork();

    print("make sure chaining is not happening");
    checkNoChaining();

    print("check that forcing sync target still works");
    forceSync();

    let config = primary.getDB("local").system.replset.findOne();
    assert.eq(false, config.settings.chainingAllowed, tojson(config));
}

replTest.stopSet();
