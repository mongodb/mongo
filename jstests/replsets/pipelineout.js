// test $out in a replicated environment
var name = "pipelineout";
var replTest = new ReplSetTest({name: name, nodes: 2});
var nodes = replTest.nodeList();

replTest.startSet();
replTest.initiate(
    {"_id": name, "members": [{"_id": 0, "host": nodes[0]}, {"_id": 1, "host": nodes[1]}]});

var primary = replTest.getPrimary().getDB(name);
var secondary = replTest.getSecondary().getDB(name);

// populate the collection
for (i = 0; i < 5; i++) {
    primary.coll.insert({x: i});
}
replTest.awaitReplication();

// run one and check for proper replication
primary.coll.aggregate({$out: "out"}).itcount();
replTest.awaitReplication();
assert.eq(primary.out.find().sort({x: 1}).toArray(), secondary.out.find().sort({x: 1}).toArray());

replTest.stopSet();
