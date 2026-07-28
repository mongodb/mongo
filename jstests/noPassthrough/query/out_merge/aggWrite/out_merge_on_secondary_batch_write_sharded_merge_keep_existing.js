/**
 * Test which verifies that $merge aggregations with whenMatched: "keepExisting", whenNotMatched: "insert"
 * and secondary read preference which write over 16 MB work as expected (especially with respect to
 * producing correctly sized write batches).
 *
 * @tags: [
 *   uses_$out,
 *   assumes_read_preference_unchanged,
 *   # TODO SERVER-132345: Enable test on TSAN variant.
 *   incompatible_disaggregated_storage_tsan,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {testOutAndMergeOnSecondaryBatchWrite} from "jstests/noPassthrough/libs/query/out_merge_on_secondary_batch_write.js";

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
testOutAndMergeOnSecondaryBatchWrite(
    st.s.getDB("db"),
    () => st.awaitReplicationOnShards(),
    "merge_keep_existing",
);
st.stop();
