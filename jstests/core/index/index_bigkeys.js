/**
 * Test interactions with big index keys. There should be no size limit for index keys.
 *
 * @tags: [
 *     # This test uses delete which is not retryable
 *     requires_non_retryable_writes,
 *     # Cannot implicitly shard accessed collections because of not being able to create unique
 *     # index using hashed shard key pattern.
 *     cannot_create_unique_index_when_using_hashed_shard_key,
 * ]
 */
import {testAllInteractionsWithBigIndexKeys} from "jstests/libs/index_builds/index_bigkeys.js";

const collName = "index_bigkeys_foreground_test";

testAllInteractionsWithBigIndexKeys(db, collName);
