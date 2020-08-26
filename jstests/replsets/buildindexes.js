// Check that buildIndexes config option is working

(function() {
// Skip db hash check because secondary will have different number of indexes due to
// buildIndexes=false on the secondary.
TestData.skipCheckDBHashes = true;
var name = "buildIndexes";
var host = getHostName();

var replTest = new ReplSetTest({name: name, nodes: 3});

var nodes = replTest.startSet();

var config = replTest.getReplSetConfig();
config.members[2].priority = 0;
config.members[2].buildIndexes = false;

replTest.initiate(config);

var primary = replTest.getPrimary().getDB(name);
var secondaryConns = replTest.getSecondaries();
var secondaries = [];
for (var i in secondaryConns) {
    secondaryConns[i].setSlaveOk();
    secondaries.push(secondaryConns[i].getDB(name));
}
replTest.awaitReplication();

primary.x.ensureIndex({y: 1});

for (i = 0; i < 100; i++) {
    primary.x.insert({x: 1, y: "abc", c: 1});
}

replTest.awaitReplication();

assert.commandWorked(secondaries[0].runCommand({count: "x"}));

var indexes = secondaries[0].stats().indexes;
assert.eq(indexes, 2, 'number of indexes');

indexes = secondaries[1].stats().indexes;
assert.eq(indexes, 1);

indexes = secondaries[0].x.stats().indexSizes;

var count = 0;
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
}());
