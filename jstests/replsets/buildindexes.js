// Check that buildIndexes config option is working

import {ReplSetTest} from "jstests/libs/replsettest.js";

// Skip db hash check because secondary will have different number of indexes due to
// buildIndexes=false on the secondary.
TestData.skipCheckDBHashes = true;
let name = "buildIndexes";
let host = getHostName();

let replTest = new ReplSetTest({name: name, nodes: 3});

let nodes = replTest.startSet();

let config = replTest.getReplSetConfig();
config.members[2].priority = 0;
config.members[2].buildIndexes = false;

replTest.initiate(config);

let primary = replTest.getPrimary().getDB(name);
let secondaryConns = replTest.getSecondaries();
let secondaries = [];
for (var i in secondaryConns) {
    secondaryConns[i].setSecondaryOk();
    secondaries.push(secondaryConns[i].getDB(name));
}
replTest.awaitReplication();

// Need to use a commitQuorum of 2 rather than the default of 'votingMembers', which includes the
// buildIndexes:false node. The index build will otherwise fail early.
assert.commandWorked(
    primary.runCommand({
        createIndexes: "x",
        indexes: [{key: {y: 1}, name: "y_1"}],
        commitQuorum: 2,
    }),
);

for (i = 0; i < 100; i++) {
    primary.x.insert({x: 1, y: "abc", c: 1});
}

replTest.awaitReplication();

assert.commandWorked(secondaries[0].runCommand({count: "x"}));

let indexes = secondaries[0].stats().indexes;
assert.eq(indexes, 2, "number of indexes");

indexes = secondaries[1].stats().indexes;
assert.eq(indexes, 1);

indexes = secondaries[0].x.stats().indexSizes;

let count = 0;
for (i in indexes) {
    count++;
    if (i == "_id_") {
        continue;
    }
    assert(i.match(/y_/));
}

assert.eq(count, 2);

indexes = secondaries[1].x.stats().indexSizes;

count = 0;
for (i in indexes) {
    count++;
}

assert.eq(count, 1);

replTest.stopSet();
