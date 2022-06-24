// Tests time-based pre-image retention policy of change stream pre-images remover job.
// @tags: [
//  requires_sharding,
// ]
(function() {
"use strict";

load("jstests/noPassthrough/libs/change_stream_pre_image_time_based_expiration_utils.js");

// Tests pre-image time-based expiration on a sharded cluster.
(function testChangeStreamPreImagesforTimeBasedExpirationOnShardedCluster() {
    const options = {
        mongos: 1,
        config: 1,
        shards: 1,
        rs: {
            nodes: 3,
        },
    };
    const st = new ShardingTest(options);
    testTimeBasedPreImageRetentionPolicy(st.s0, st.rs0.getPrimary());
    st.stop();
})();
}());
