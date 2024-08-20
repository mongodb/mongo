/*
 * Ensure options object passed to ShardingTest is not mutated.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const opts = {
    setParameter: {},
};

try {
    const st = new ShardingTest({
        mongos: [opts],
        config: [opts],
        rs: {nodes: [opts]},
        shards: 1,
    });
    st.stop();
} catch (e) {
    assert(false, `ShardingTest threw an error: ${tojson(e)}`);
} finally {
    assert.eq(opts, {setParameter: {}});
}