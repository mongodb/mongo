// test $out in a replicated environment
import {ReplSetTest} from "jstests/libs/replsettest.js";

let name = "pipelineout";
let replTest = new ReplSetTest({name: name, nodes: 2});
let nodes = replTest.nodeList();

replTest.startSet();
replTest.initiate({
    "_id": name,
    "members": [
        {"_id": 0, "host": nodes[0]},
        {"_id": 1, "host": nodes[1]},
    ],
});

let primary = replTest.getPrimary().getDB(name);
let secondary = replTest.getSecondary().getDB(name);

// populate the collection
for (let i = 0; i < 5; i++) {
    primary.coll.insert({x: i});
}
replTest.awaitReplication();

// run one and check for proper replication
primary.coll.aggregate({$out: "out"}).itcount();
replTest.awaitReplication();
assert.eq(primary.out.find().sort({x: 1}).toArray(), secondary.out.find().sort({x: 1}).toArray());

replTest.stopSet();
