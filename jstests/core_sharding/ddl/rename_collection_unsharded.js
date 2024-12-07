/**
 * A set of sharded-cluster specific test cases involving unsharded source/target namespaces.
 *
 * @tags: [
 *  # This test requires full control on the lifecycle of an unsharded collection and its placement.
 *  assumes_unsharded_collection,
 *  assumes_no_implicit_collection_creation_after_drop,
 *  assumes_balancer_off,
 *   # Note: The test cases requires a minimum of 2 shards to qualify the behaviour depending on
 *   # data-bearing shards; a cluster with more than 3 shards adds further coverage concerning the
 *   # behaviour of non-data bearing shards, that still need to deal with:
 *   # - Locally unknown source collections to rename
 *   # - Locally unknown target collections to drop
 *  requires_2_or_more_shards,
 *  ]
 */

import {
    getRandomShardName,
    setupTestDatabase
} from "jstests/libs/sharded_cluster_fixture_helpers.js";
import {getUUIDFromConfigCollections} from "jstests/libs/uuid_util.js";
import {
    moveDatabaseAndUnshardedColls
} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";

const fromCollName = 'from';
const toCollName = 'to';

/**
 * Initialize a sharded collection with key 'x:1' and 2 chunks distributed on 2 different nodes -
 * each containing 1 document. Assumes that dbName already exists.
 */
function setupShardedCollection(conn, dbName, collName) {
    const testDB = conn.getSiblingDB(dbName);
    const primaryShardId = conn.getSiblingDB(dbName).getDatabasePrimaryShardId();
    assert(primaryShardId,
           `Must explicitly invoke createDatabase(${dbName}) before calling this method`);
    const ns = dbName + '.' + collName;
    const nonPrimaryShardId = getRandomShardName(db, [primaryShardId]);
    assert.commandWorked(conn.adminCommand({shardCollection: ns, key: {x: 1}}));

    const coll = testDB.getCollection(collName);
    coll.insert({x: 0});
    coll.insert({x: 2});

    assert.commandWorked(conn.adminCommand({
        moveRange: ns,
        min: {x: 0},
        max: {x: MaxKey},
        toShard: nonPrimaryShardId,
    }));
}

/**
 * Performs a rename to `toNs` with the provided options and get sure it succeeds/fails as expected.
 * The function assumes that
 * - `fromCollName` and `toCollName` belong to the same `dbName`
 * - `fromCollName` refers to a sharded collection created through setupShardedCollection().
 */
function testRename(conn, dbName, fromCollName, toCollName, dropTarget, mustFail) {
    const testDB = conn.getSiblingDB(dbName);
    const res = testDB[fromCollName].renameCollection(toCollName, dropTarget);
    if (mustFail) {
        assert.commandFailed(res);
        return;
    }

    assert.commandWorked(res);

    const toUUID = getUUIDFromConfigCollections(conn, `${dbName}.${toCollName}`);
    const chunks = conn.getSiblingDB('config').chunks.find({uuid: toUUID});
    const chunk0 = chunks.next();
    const chunk1 = chunks.next();

    assert(!chunks.hasNext(), 'Target collection expected to have exactly 2 chunks');
    assert(chunk0.shard != chunk1.shard, 'Chunks expected to be on different shards');

    const toColl = testDB.getCollection(toCollName);
    assert.eq(toColl.find({x: 0}).itcount(), 1, 'Expected exactly one document on the shard');
    assert.eq(toColl.find({x: 2}).itcount(), 1, 'Expected exactly one document on the shard');
}

{
    jsTest.log("Renaming unsharded collection to a different db with same primary shard");
    const primaryShard = getRandomShardName(db);
    const fromDB = setupTestDatabase(db, 'firstDBOnSamePrimary', primaryShard);
    const toDB = setupTestDatabase(db, 'secondDBOnSamePrimary', primaryShard);

    let unshardedColl = fromDB[fromCollName];
    unshardedColl.insert({x: 1});

    assert.commandWorked(fromDB.adminCommand(
        {renameCollection: unshardedColl.getFullName(), to: `${toDB.getName()}.${toCollName}`}));
    assert.eq(0, unshardedColl.countDocuments({}));
    assert.eq(1, db.getSiblingDB(toDB)[toCollName].countDocuments({}));
}

{
    jsTest.log('Renaming unsharded collection to a different db with different primary shard');
    const aPrimaryShard = getRandomShardName(db);
    const anotherPrimaryShard = getRandomShardName(db, /* exclude =*/[aPrimaryShard]);

    const testDB1 = setupTestDatabase(db, 'firstDBOnAPrimary', aPrimaryShard);
    const testDB2 = setupTestDatabase(db, 'secondDBOnADifferentPrimary', anotherPrimaryShard);

    let unshardedColl = testDB1[fromCollName];
    unshardedColl.insert({x: 1});

    assert.commandFailedWithCode(
        db.adminCommand({
            renameCollection: unshardedColl.getFullName(),
            to: `${testDB2.getName()}.${toCollName}`
        }),
        [ErrorCodes.CommandFailed],
        "Source and destination collections must be on the same database.");
}

// Test that the rename of an unsharded collection across DBs after performing movePrimary.
{
    jsTest.log(
        'Renaming an unsharded collection across DBs under the same primary after performing movePrimary');
    const originalPrimaryShard = getRandomShardName(db);
    const anotherPrimaryShard = getRandomShardName(db, /* exclude =*/[originalPrimaryShard]);

    const testDB1 = setupTestDatabase(db, 'firstDBOnSamePrimary', originalPrimaryShard);
    const testDB2 = setupTestDatabase(db, 'secondDBOnSamePrimary', originalPrimaryShard);
    let unshardedFromColl = testDB1[fromCollName];
    let unshardedToColl = testDB2[toCollName];
    unshardedFromColl.insert({a: 0});

    // Move both databases to the same destination primary shard and run renameCollection.
    moveDatabaseAndUnshardedColls(testDB1, anotherPrimaryShard);
    moveDatabaseAndUnshardedColls(testDB2, anotherPrimaryShard);

    assert.commandWorked(db.adminCommand({
        renameCollection: unshardedFromColl.getFullName(),
        to: unshardedToColl.getFullName(),
    }));

    // Undo the previous movePrimaries and verify that  renameCollection still works as expected.
    moveDatabaseAndUnshardedColls(testDB1, originalPrimaryShard);
    moveDatabaseAndUnshardedColls(testDB2, originalPrimaryShard);

    assert.commandWorked(db.adminCommand({
        renameCollection: unshardedToColl.getFullName(),
        to: unshardedFromColl.getFullName(),
    }));
}

{
    jsTest.log('Rename to existing unsharded target collection with dropTarget=true');
    const dbName = 'testRenameToExistingUnshardedCollection';
    const testDB = setupTestDatabase(db, dbName);
    setupShardedCollection(testDB, dbName, fromCollName);

    const toColl = testDB.getCollection(toCollName);
    toColl.insert({a: 0});

    testRename(
        testDB, dbName, fromCollName, toCollName, true /* dropTarget */, false /* mustFail */);
}

{
    jsTest.log('Rename to existing unsharded target collection with dropTarget=false must fail');
    const dbName = 'testRenameToUnshardedCollectionWithoutDropTarget';
    const testDB = setupTestDatabase(db, dbName);
    setupShardedCollection(testDB, dbName, fromCollName);

    const toColl = testDB.getCollection(toCollName);
    toColl.insert({a: 0});

    testRename(
        testDB, dbName, fromCollName, toCollName, false /* dropTarget */, true /* mustFail*/);
}

{
    jsTest.log('Rename unsharded collection to sharded target collection with dropTarget=true');
    const dbName = 'testRenameUnshardedToShardedTargetCollection';
    const testDB = setupTestDatabase(db, dbName);
    const unshardedFromColl = testDB.getCollection(fromCollName);
    unshardedFromColl.insert({a: 0});

    setupShardedCollection(testDB, dbName, toCollName);
    const shardedToColl = testDB.getCollection(toCollName);

    assert.commandWorked(unshardedFromColl.renameCollection(toCollName, true /* dropTarget */));

    // Source collection just has documents with field `a`
    assert.eq(
        shardedToColl.find({a: {$exists: true}}).itcount(), 1, 'Expected one source document');
    // Source collection has no documents with field `x` (belonging to the dropped target).
    assert.eq(
        shardedToColl.find({x: {$exists: true}}).itcount(), 0, 'Expected no target documents');
}

// TODO SERVER-98025 move this test case to jstests/core/ddl/rename_collection.js
{
    jsTest.log('[C2C] Rename of existing collection with extra UUID parameter must succeed');
    const dbName = 'testRenameToUnshardedCollectionWithSourceUUID';
    const testDB = setupTestDatabase(db, dbName);

    const unshardedFromColl = testDB.getCollection(fromCollName);
    unshardedFromColl.insert({a: 0});

    const unshardedToColl = testDB.getCollection(toCollName);
    unshardedToColl.insert({b: 0});

    const sourceUUID = assert.commandWorked(testDB.runCommand({listCollections: 1}))
                           .cursor.firstBatch.find(c => c.name === fromCollName)
                           .info.uuid;

    // The command succeeds when the correct UUID is provided.
    assert.commandWorked(testDB.adminCommand({
        renameCollection: unshardedFromColl.getFullName(),
        to: unshardedToColl.getFullName(),
        dropTarget: true,
        collectionUUID: sourceUUID,
    }));
}
