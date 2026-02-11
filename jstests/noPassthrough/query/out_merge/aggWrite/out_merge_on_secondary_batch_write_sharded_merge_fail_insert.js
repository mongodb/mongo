/**
 * Test which verifies that $merge aggregations with whenMatched: "fail", whenNotMatched: "insert"
 * and secondary read preference correctly fail with DuplicateKey when documents already exist
 * in the target collection.
 *
 * @tags: [uses_$out, assumes_read_preference_unchanged]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {testOutAndMergeOnSecondaryBatchWrite} from "jstests/noPassthrough/libs/query/out_merge_on_secondary_batch_write.js";

const st = new ShardingTest({shards: 1, rs: {nodes: 2}});
testOutAndMergeOnSecondaryBatchWrite(st.s.getDB("db"), () => st.awaitReplicationOnShards(), "merge_fail_insert");
st.stop();
