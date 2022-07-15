/**
 * Tests that the balancer should not be moving chunks outside of the active window.
 * The outline of the test goes like this:
 * 1. Disable balancer to make sure that it will not try to move chunk on the setup phase.
 * 2. Setup the sharded collections by splitting it into 16 chunks. Since none of the chunks
 *    are being moved, all of them will end up on a single shard.
 * 3. Turn the balancer setting to on and set the active balancing window to a time that is
 *    outside of the current time at the same time.
 * 4. Make sure that no chunks have moved for at least one balancing round.
 * 5. Reset the active balancing window to a setting that overlaps the current time and make
 *    sure that some chunks are moved.
 */
(function() {
'use strict';

load("jstests/sharding/libs/find_chunks_util.js");

/**
 * Simple representation for wall clock time. Hour and minutes should be integers.
 */
var HourAndMinute = function(hour, minutes) {
    return {
        /**
         * Returns a new HourAndMinute object with the amount of hours added.
         * Amount can be negative.
         */
        addHour: function(amount) {
            var newHour = (hour + amount) % 24;
            if (newHour < 0) {
                newHour += 24;
            }

            return new HourAndMinute(newHour, minutes);
        },

        /**
         * Returns a string representation that is compatible with the format for the balancer
         * window settings.
         */
        toString: function() {
            var minStr = (minutes < 10) ? ('0' + minutes) : ('' + minutes);
            var hourStr = (hour < 10) ? ('0' + hour) : ('' + hour);
            return hourStr + ':' + minStr;
        }
    };
};

const st = new ShardingTest({shards: 2, other: {chunkSize: 1, enableAutoSplit: false}});
const dbName = 'test';
const collName = 'user';
const ns = dbName + '.' + collName;
const configDB = st.s.getDB('config');
assert.commandWorked(configDB.adminCommand({enableSharding: dbName}));
assert.commandWorked(configDB.adminCommand({shardCollection: ns, key: {_id: 1}}));

const bigString = 'X'.repeat(1024 * 1024);  // 1MB
const coll = st.s.getDB(dbName).getCollection(collName);
for (var x = 0; x < 150; x += 10) {
    coll.insert({_id: x, s: bigString});
    configDB.adminCommand({split: ns, middle: {_id: x}});
}

var shard0Chunks =
    findChunksUtil.findChunksByNs(configDB, ns, {shard: st.shard0.shardName}).count();

var startDate = new Date();
var hourMinStart = new HourAndMinute(startDate.getHours(), startDate.getMinutes());
assert.commandWorked(
    configDB.settings.update({_id: 'balancer'},
                             {
                                 $set: {
                                     activeWindow: {
                                         start: hourMinStart.addHour(-2).toString(),
                                         stop: hourMinStart.addHour(-1).toString()
                                     },
                                 }
                             },
                             true));
st.startBalancer();

st.awaitBalancerRound();

var shard0ChunksAfter =
    findChunksUtil.findChunksByNs(configDB, ns, {shard: st.shard0.shardName}).count();
assert.eq(shard0Chunks, shard0ChunksAfter);

assert.commandWorked(configDB.settings.update(
    {_id: 'balancer'},
    {
        $set: {
            activeWindow: {start: hourMinStart.toString(), stop: hourMinStart.addHour(2).toString()}
        }
    },
    true));

st.awaitBalancerRound();

shard0ChunksAfter =
    findChunksUtil.findChunksByNs(configDB, ns, {shard: st.shard0.shardName}).count();
assert.neq(shard0Chunks, shard0ChunksAfter);

st.stop();
})();
