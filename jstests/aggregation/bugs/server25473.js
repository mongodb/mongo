/**
 * This test was designed to stress special logic in the mongos for when the database has sharding
 * enabled, but the collection is not sharded.
 *
 * TODO SERVER-25430 This test can be removed once we have a passthrough to stress this logic.
 */
(function() {
    'use strict';
    var st = new ShardingTest({mongos: 1, shards: 1});

    st.s.adminCommand({enableSharding: 'test'});
    st.s.getDB('test').runCommand(
        {aggregate: 'article', pipeline: [{$project: {all: {$min: 10}}}]});

    st.stop();
}());
