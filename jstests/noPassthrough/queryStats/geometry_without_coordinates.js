// This test was designed to reproduce SERVER-77430. There was a mistaken assertion in a parser that
// we are interested in proving will not fail here.
// @tags: [featureFlagQueryStats]
(function() {
"use strict";

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 1},
    mongosOptions: {
        setParameter: {
            internalQueryStatsRateLimit: -1,
        }
    },
});
const coll = st.s.getDB("test").geometry_without_coordinates;
// This is a query that once mistakenly threw an error.
assert.doesNotThrow(() => coll.find({geo: {$geoIntersects: {$geometry: {x: 40, y: 5}}}}).itcount());
st.stop();
}());
