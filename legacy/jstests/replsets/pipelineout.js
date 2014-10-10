// test $out in a replicated environment
var name = "pipelineout";
var replTest = new ReplSetTest( {name: name, nodes: 2} );
var nodes = replTest.nodeList();

replTest.startSet();
replTest.initiate({"_id" : name,
                   "members" : [
                         {"_id" : 0, "host" : nodes[0], "priority" : 5},
                         {"_id" : 1, "host" : nodes[1]}
     ]});

var primary = replTest.getMaster().getDB(name);
var secondary = replTest.liveNodes.slaves[0].getDB(name);

// populate the collection
for (i=0; i<5; i++) {
    primary.in.insert({x:i});
}
replTest.awaitReplication();

// make sure $out cannot be run on a secondary
assert.throws(function() {
                  secondary.in.aggregate({$out: "out"}).itcount;
        });
// even if slaveOk
secondary.setSlaveOk();
assert.throws(function() {
                  secondary.in.aggregate({$out: "out"}).itcount;
        });

// run one and check for proper replication
primary.in.aggregate({$out: "out"}).itcount;
replTest.awaitReplication();
assert.eq(primary.out.find().toArray(), secondary.out.find().toArray());
