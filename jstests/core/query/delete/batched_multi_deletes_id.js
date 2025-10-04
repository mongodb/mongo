/**
 * Tests batch-deleting a large range of data using predicate on the _id field.
 * TODO SERVER-97166 Re-evaluate if this test does not need to rely on getMores
 * This test does not rely on getMores on purpose.
 *
 * @tags: [
 *   does_not_support_retryable_writes,
 *   # TODO (SERVER-55909): make WUOW 'groupOplogEntries' the only mode of operation.
 *   does_not_support_transactions,
 *   no_selinux,
 *   requires_non_retryable_writes,
 * ]
 */

import {runBatchedMultiDeletesTest} from "jstests/core/query/delete/batched_multi_deletes.js";

runBatchedMultiDeletesTest(db[jsTestName()], {_id: {$gte: 0}});
