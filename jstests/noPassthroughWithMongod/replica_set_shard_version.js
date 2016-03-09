// Tests whether a Replica Set in a mongos cluster can cause versioning problems

jsTestLog("Starting sharded cluster...");

var st = new ShardingTest({shards: 1, mongos: 2, other: {rs: true}});

// Uncomment to stop the balancer, since the balancer usually initializes the shard automatically
// SERVER-4921 is otherwise hard to manifest
// st.stopBalancer()

var mongosA = st.s0;
var mongosB = st.s1;
var shard = st.shard0;

coll = mongosA.getCollection(jsTestName() + ".coll");

// Wait for primary and then initialize shard SERVER-5130
st.rs0.getPrimary();
coll.findOne();

var sadmin = shard.getDB("admin");
assert.throws(function() {
    sadmin.runCommand({replSetStepDown: 3000, force: true});
});

st.rs0.getPrimary();

mongosA.getDB("admin").runCommand({setParameter: 1, traceExceptions: true});

try {
    // This _almost_ always fails, unless the new primary is already detected.   If if fails, it
    // should
    // mark the master as bad, so mongos will reload the replica set master next request
    // TODO: Can we just retry and succeed here?
    coll.findOne();
} catch (e) {
    print("This error is expected : ");
    printjson(e);
}

jsTest.log("Running query which should succeed...");

// This should always succeed without throwing an error
coll.findOne();

mongosA.getDB("admin").runCommand({setParameter: 1, traceExceptions: false});

// now check secondary

assert.throws(function() {
    sadmin.runCommand({replSetStepDown: 3000, force: true});
});

// Can't use the mongosB - SERVER-5128
other = new Mongo(mongosA.host);
other.setSlaveOk(true);
other = other.getCollection(jsTestName() + ".coll");

print("eliot: " + tojson(other.findOne()));

st.stop();
