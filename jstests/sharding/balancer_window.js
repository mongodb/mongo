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

    var st = new ShardingTest({shards: 2});
    var configDB = st.s.getDB('config');
    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.user', key: {_id: 1}}));

    for (var x = 0; x < 150; x += 10) {
        configDB.adminCommand({split: 'test.user', middle: {_id: x}});
    }

    var shard0Chunks = configDB.chunks.find({ns: 'test.user', shard: 'shard0000'}).count();

    var startDate = new Date();
    var hourMinStart = new HourAndMinute(startDate.getHours(), startDate.getMinutes());
    assert.writeOK(configDB.settings.update({_id: 'balancer'},
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

    var shard0ChunksAfter = configDB.chunks.find({ns: 'test.user', shard: 'shard0000'}).count();
    assert.eq(shard0Chunks, shard0ChunksAfter);

    assert.writeOK(configDB.settings.update(
        {_id: 'balancer'},
        {
          $set: {
              activeWindow:
                  {start: hourMinStart.toString(), stop: hourMinStart.addHour(2).toString()}
          }
        },
        true));

    st.awaitBalancerRound();

    shard0ChunksAfter = configDB.chunks.find({ns: 'test.user', shard: 'shard0000'}).count();
    assert.neq(shard0Chunks, shard0ChunksAfter);

    st.stop();
})();
