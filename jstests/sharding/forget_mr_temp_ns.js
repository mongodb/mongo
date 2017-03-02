//
// Tests whether we forget M/R's temporary namespaces for sharded output
//

var st = new ShardingTest({shards: 1, bongos: 1});

var bongos = st.s0;
var admin = bongos.getDB("admin");
var coll = bongos.getCollection("foo.bar");
var outputColl = bongos.getCollection((coll.getDB() + "") + ".mrOutput");

var bulk = coll.initializeUnorderedBulkOp();
for (var i = 0; i < 10; i++) {
    bulk.insert({_id: i, even: (i % 2 == 0)});
}
assert.writeOK(bulk.execute());

var map = function() {
    emit(this.even, 1);
};
var reduce = function(key, values) {
    return Array.sum(values);
};

out = coll.mapReduce(map, reduce, {out: {reduce: outputColl.getName(), sharded: true}});

printjson(out);
printjson(outputColl.find().toArray());

var bongodThreadStats = st.shard0.getDB("admin").runCommand({shardConnPoolStats: 1}).threads;
var bongosThreadStats = admin.runCommand({shardConnPoolStats: 1}).threads;

printjson(bongodThreadStats);
printjson(bongosThreadStats);

var checkForSeenNS = function(threadStats, regex) {
    for (var i = 0; i < threadStats.length; i++) {
        var seenNSes = threadStats[i].seenNS;
        for (var j = 0; j < seenNSes.length; j++) {
            assert(!(regex.test(seenNSes)));
        }
    }
};

checkForSeenNS(bongodThreadStats, /^foo.tmp/);
checkForSeenNS(bongosThreadStats, /^foo.tmp/);

st.stop();
