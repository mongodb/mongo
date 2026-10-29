/*
 * Test the rename command against unsplittable collections.
 * @tags: [
 *   # Requires stable collection placement
 *   assumes_balancer_off,
 *   requires_2_or_more_shards,
 *   # It uses rename command that is not retriable.
 *   # After succeeding, any subsequent attempt will fail
 *   # because the source namespace does not exist anymore.
 *   requires_non_retryable_commands,
 *  # This test expects explicit control over the tracking state of unsharded collections
 *  assumes_unsharded_collection,
 * ]
 */

import {
    getRandomShardName,
    setupTestDatabase,
} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";
import {testRenameUnsplittableCollection} from "jstests/libs/cluster_helpers/rename_collection_across_dbs_helpers.js";

// Setup two databases sharing the same primary shard.
const dbName = "testDb";
const anotherDbName = "anotherTestDb";
const primaryShard = getRandomShardName(db);
const nonPrimaryShard = getRandomShardName(db, [primaryShard]);
const testDB = setupTestDatabase(db, dbName, primaryShard);
const anotherTestDB = setupTestDatabase(db, anotherDbName, primaryShard);

const configDb = db.getSiblingDB("config");

// 1. Rename collection  test:located on the primary shard
testRenameUnsplittableCollection(configDb, db, "collFrom1", db, "collTo1", primaryShard);

// 2. Rename collection  test:located on the primary shard when target exists
testRenameUnsplittableCollection(
    configDb,
    testDB,
    "collFrom2",
    testDB,
    "collTo2",
    primaryShard,
    true /*collToShouldExist*/,
    primaryShard,
);

// 3. Rename collection  test:not located on the primary shard
testRenameUnsplittableCollection(configDb, db, "collFrom3", db, "collTo3", nonPrimaryShard);

// 4. Rename collection  test:not located on the primary shard when target exists
testRenameUnsplittableCollection(
    configDb,
    testDB,
    "collFrom4",
    testDB,
    "collTo4",
    nonPrimaryShard,
    true /*collToShouldExist*/,
    nonPrimaryShard,
);

// 5. Rename collection  test:when target exists on another shard
testRenameUnsplittableCollection(
    configDb,
    testDB,
    "collFrom5",
    testDB,
    "collTo5",
    nonPrimaryShard,
    true /*collToShouldExist*/,
    primaryShard,
);

// 6. Rename collection  test:located on the primary shard across DBs
testRenameUnsplittableCollection(
    configDb,
    testDB,
    "collFrom6",
    anotherTestDB,
    "collTo6",
    primaryShard,
);

// 7. Rename collection  test:not located on the primary shard across DBs
testRenameUnsplittableCollection(
    configDb,
    testDB,
    "collFrom7",
    anotherTestDB,
    "collTo7",
    nonPrimaryShard,
);

// 8. Rename collection  test:located on the primary shard across DBs when target exists
testRenameUnsplittableCollection(
    configDb,
    testDB,
    "collFrom8",
    anotherTestDB,
    "collTo8",
    primaryShard,
    true /*collToShouldExist*/,
    primaryShard,
);

// 9. Rename collection  test:not located on the primary shard across DBs when target exists
testRenameUnsplittableCollection(
    configDb,
    testDB,
    "collFrom9",
    anotherTestDB,
    "collTo9",
    nonPrimaryShard,
    true /*collToShouldExist*/,
    nonPrimaryShard,
);

// 10. Rename collection  test:not located on the primary shard across DBs when target exists
testRenameUnsplittableCollection(
    configDb,
    testDB,
    "collFrom10",
    anotherTestDB,
    "collTo10",
    primaryShard,
    true /*collToShouldExist*/,
    nonPrimaryShard,
);
