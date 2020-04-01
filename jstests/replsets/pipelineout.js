// test $out in a replicated environment
var name = "pipelineout";

// When calling replTest.getPrimary(), the slaveOk bit will be set to true, which will result in
// running all commands with a readPreference of 'secondaryPreferred'. This is problematic in a
// mixed 4.2/4.4 Replica Set cluster as running $out/$merge  with non-primary read preference
// against a cluster in FCV 4.2 is not allowed. As such, setting this test option provides a
// means to ensure that the commands in this test file run with readPreference 'primary'.
TestData.shouldSkipSettingSlaveOk = true;

var replTest = new ReplSetTest({name: name, nodes: 2});
var nodes = replTest.nodeList();

replTest.startSet();
replTest.initiate(
    {"_id": name, "members": [{"_id": 0, "host": nodes[0]}, {"_id": 1, "host": nodes[1]}]});

var primary = replTest.getPrimary().getDB(name);
var secondary = replTest._slaves[0].getDB(name);

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