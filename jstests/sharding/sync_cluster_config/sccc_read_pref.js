/**
 * Simple test that makes sure that running a command with read preference settings on a SCCC setup
 * works.
 */
(function() {
    'use strict';

    // Start a sharding cluster with a single shard, which has one node
    var st = new ShardingTest({shards: 1, config: 3, other: {sync: true}});
    st.stopBalancer();

    st.s.setReadPref('secondaryPreferred');
    var count = st.s.getDB('config').mongos.count();

    assert.eq(1, count);

    st.stop();
})();
