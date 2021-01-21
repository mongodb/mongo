/**
 * This test is labeled resource intensive because its total io_write is 95MB compared to a median
 * of 5MB across all sharding tests in wiredTiger.
 * @tags: [resource_intensive]
 */
(function() {
"use strict";

load("jstests/sharding/libs/find_chunks_util.js");

var st = new ShardingTest({name: 'migrateBig_balancer', shards: 2, other: {enableBalancer: true}});
var mongos = st.s;
var admin = mongos.getDB("admin");
var db = mongos.getDB("test");
var coll = db.getCollection("stuff");

assert.commandWorked(admin.runCommand({enablesharding: coll.getDB().getName()}));
st.ensurePrimaryShard(coll.getDB().getName(), st.shard1.shardName);

var data = "x";
var nsq = 16;
var n = 255;

for (var i = 0; i < nsq; i++)
    data += data;

var dataObj = {};
for (var i = 0; i < n; i++)
    dataObj["data-" + i] = data;

var bulk = coll.initializeUnorderedBulkOp();
for (var i = 0; i < 40; i++) {
    bulk.insert({data: dataObj});
}

assert.commandWorked(bulk.execute());
assert.eq(40, coll.count(), "prep1");

assert.commandWorked(admin.runCommand({shardcollection: "" + coll, key: {_id: 1}}));
st.printShardingStatus();

assert.lt(5,
          findChunksUtil.findChunksByNs(mongos.getDB("config"), "test.stuff").count(),
          "not enough chunks");

assert.soon(() => {
    const aggMatch = (function() {
        const collMetadata = mongos.getDB("config").collections.findOne({_id: "test.stuff"});
        if (collMetadata.timestamp) {
            return {$match: {uuid: collMetadata.uuid}};
        } else {
            return {$match: {ns: "test.stuff"}};
        }
    }());
    let res = mongos.getDB("config")
                  .chunks.aggregate([aggMatch, {$group: {_id: "$shard", nChunks: {$sum: 1}}}])
                  .toArray();
    printjson(res);
    return res.length > 1 && Math.abs(res[0].nChunks - res[1].nChunks) <= 3;
}, "never migrated", 10 * 60 * 1000, 1000);

st.stop();
})();
