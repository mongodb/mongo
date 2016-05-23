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

    /**
     * Waits until at least one balancing round has passed.
     *
     * Note: This relies on the fact that the balancer pings the config.mongos document every round.
     */
    var waitForAtLeastOneBalanceRound = function(mongosHost, timeoutMS) {
        var mongos = new Mongo(mongosHost);
        var configDB = mongos.getDB('config');

        // Wait for ts to change twice because:
        // 1st: for start of the balancing round.
        // 2nd: for the start of the next round, which implies that the previous one has ended.
        var waitForTSChangeNTimes = 2;
        var lastPing = new Date(0);

        assert.soon(
            function() {
                // Note: The balancer pings twice, once with { waiting: false } at the beginning
                // and another { waiting: true } at the end. Poll for the negative edge since
                // the smallest granurality should be a second, if for some reason the interval
                // became less than a second, it can cause this to miss the negative edge and
                // wake it wait longer than it should.
                var currentPing = configDB.mongos.findOne({_id: mongosHost, waiting: true});
                if (currentPing == null) {
                    return false;
                }

                if (currentPing.ping.valueOf() != lastPing.valueOf()) {
                    waitForTSChangeNTimes--;
                    lastPing = currentPing.ping;
                }

                return waitForTSChangeNTimes <= 0;
            },
            'Timed out waiting for mongos ping to change ' + waitForTSChangeNTimes + ' more times',
            timeoutMS,
            500);
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
                                                  stopped: false,
                                              }
                                            },
                                            true));

    waitForAtLeastOneBalanceRound(st.s.host, 60 * 1000);

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

    waitForAtLeastOneBalanceRound(st.s.host, 60 * 1000);

    shard0ChunksAfter = configDB.chunks.find({ns: 'test.user', shard: 'shard0000'}).count();
    assert.neq(shard0Chunks, shard0ChunksAfter);

    st.stop();
})();
