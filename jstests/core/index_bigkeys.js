/**
 * Test interactions with big index keys. There should be no size limit for index keys.
 *
 * assumes_no_implicit_index_creation: Cannot implicitly shard accessed collections because of extra
 * shard key index in sharded collection.
 * requires_non_retryable_writes: This test uses delete which is not retryable
 * @tags: [assumes_no_implicit_index_creation, requires_non_retryable_writes]
 */
(function() {
"use strict";

load("jstests/libs/index_bigkeys.js");

const collName = "index_bigkeys_foreground_test";

testAllInteractionsWithBigIndexKeys(db, collName, false);
}());
