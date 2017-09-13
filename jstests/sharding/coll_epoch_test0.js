// Tests whether a split and a migrate in a sharded cluster preserve the epoch

var st = new ShardingTest({shards: 2, mongos: 1});
// Balancer is by default stopped, thus it will not interfere

var config = st.s.getDB("config");
var admin = st.s.getDB("admin");
var coll = st.s.getCollection("foo.bar");

// First enable sharding
admin.runCommand({enableSharding: coll.getDB() + ""});
st.ensurePrimaryShard(coll.getDB().getName(), 'shard0001');
admin.runCommand({shardCollection: coll + "", key: {_id: 1}});

var primary = config.databases.find({_id: coll.getDB() + ""}).primary;
var notPrimary = null;
config.shards.find().forEach(function(doc) {
    if (doc._id != primary)
        notPrimary = doc._id;
});

var createdEpoch = null;
var checkEpochs = function() {
    config.chunks.find({ns: coll + ""}).forEach(function(chunk) {

        // Make sure the epochs exist, are non-zero, and are consistent
        assert(chunk.lastmodEpoch);
        print(chunk.lastmodEpoch + "");
        assert.neq(chunk.lastmodEpoch + "", "000000000000000000000000");
        if (createdEpoch == null)
            createdEpoch = chunk.lastmodEpoch;
        else
            assert.eq(createdEpoch, chunk.lastmodEpoch);

    });
};

checkEpochs();

// Now do a split
printjson(admin.runCommand({split: coll + "", middle: {_id: 0}}));

// Check all the chunks for epochs
checkEpochs();

// Now do a migrate
printjson(admin.runCommand({moveChunk: coll + "", find: {_id: 0}, to: notPrimary}));

// Check all the chunks for epochs
checkEpochs();

st.stop();
