/**
 * Tests catalog shard topology.
 */
(function() {
"use strict";

const st = new ShardingTest({
    shards: 0,
    config: 1,
    configOptions: {setParameter: {featureFlagCatalogShard: true}},
});

st.stop();
}());
