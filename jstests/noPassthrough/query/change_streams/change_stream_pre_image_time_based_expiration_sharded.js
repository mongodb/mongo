// Tests time-based pre-image retention policy of change stream pre-images remover job.
// @tags: [
//  requires_sharding,
// ]
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {testTimeBasedPreImageRetentionPolicy} from "jstests/noPassthrough/libs/change_stream_pre_image_time_based_expiration_utils.js";

// Tests pre-image time-based expiration on a sharded cluster.
(function testChangeStreamPreImagesforTimeBasedExpirationOnShardedCluster() {
    const options = {
        mongos: 1,
        config: 1,
        shards: 1,
        rs: {
            nodes: 3,
            // Test expects an exact number of pre-images to be deleted. Thus, the pre-images
            // truncate markers must only contain 1 document at most.
            setParameter: {preImagesCollectionTruncateMarkersMinBytes: 1},
        },
    };
    const st = new ShardingTest(options);
    testTimeBasedPreImageRetentionPolicy(st.s0, st.rs0.getPrimary());
    st.stop();
})();
