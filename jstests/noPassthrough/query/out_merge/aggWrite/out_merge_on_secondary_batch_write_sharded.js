/**
 * Test which verifies that $out/$merge aggregations with secondary read preference which write
 * over 16 MB work as expected (especially with respect to producing correctly sized write batches).
 *
 * @tags: [uses_$out, assumes_read_preference_unchanged]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    testOutAndMergeOnSecondaryBatchWrite
} from "jstests/noPassthrough/libs/query/out_merge_on_secondary_batch_write.js";

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
testOutAndMergeOnSecondaryBatchWrite(st.s.getDB("db"), () => st.awaitReplicationOnShards());
st.stop();
