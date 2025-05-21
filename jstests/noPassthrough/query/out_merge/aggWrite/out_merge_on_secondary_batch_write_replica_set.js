/**
 * Test which verifies that $out/$merge aggregations with secondary read preference which write
 * over 16 MB work as expected (especially with respect to producing correctly sized write batches).
 *
 * @tags: [uses_$out, assumes_read_preference_unchanged]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    testOutAndMergeOnSecondaryBatchWrite
} from "jstests/noPassthrough/libs/query/out_merge_on_secondary_batch_write.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
testOutAndMergeOnSecondaryBatchWrite(new Mongo(rst.getURL()).getDB("db"),
                                     () => rst.awaitReplication());
rst.stopSet();
