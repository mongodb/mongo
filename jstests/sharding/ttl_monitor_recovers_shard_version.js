/**
 * Test that a deletion triggered by a TTL index recovers the sharding filtering metadata
 *
 * * @tags: [
 * # Needed since previous versions didn't require the sharding filtering information to filter
 * # direct writes to shards.
 * requires_fcv_53,
 * # It requires persistence because it assumes shards will still have their data after restarting.
 * requires_persistence
 * ]
 */

(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

var st = new ShardingTest({
    mongos: 1,
    shards: 1,
    rs: {
        nodes: 1,
        // Reducing the TTL Monitor sleep
        setParameter: {ttlMonitorSleepSecs: 5}
    }
});
var kDbName = 'db';
var kCollName = 'foo';

var mongos = st.s0;
var shard0 = st.shard0.shardName;

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

var ns = kDbName + '.' + kCollName;

assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {a: 1}}));
assert.commandWorked(
    mongos.getDB(kDbName)[kCollName].createIndex({b: 1}, {expireAfterSeconds: 20}));

for (let i = 0; i < 20; ++i) {
    mongos.getDB(kDbName)[kCollName].insert({a: i, b: new Date()});
}

// At this point the shard is restarted to have an unknown filtering metadata
st.restartShardRS(0);

// The find is perfomed directly against the shard to avoid to force a refresh as part of the shard
// versioning protocol
assert.soon(() => st.shard0.getDB(kDbName)[kCollName].find({}).count() == 0);

st.stop();
})();
