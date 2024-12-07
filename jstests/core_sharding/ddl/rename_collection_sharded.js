/**
 * A set of test cases around renaming sharded collections in a multi-shard cluster.
 * @tags: [
 *   # Test assertions require collection placement stability
 *   assumes_balancer_off,
 *   # Note: The test cases requires a minimum of 2 shards to qualify the behaviour depending on
 *   # data-bearing shards; a cluster with more than 3 shards adds further coverage concerning the
 *   # behaviour of non-data bearing shards, that still need to deal with:
 *   # - Locally unknown source collections to rename
 *   # - Locally unknown target collections to drop
 *   requires_2_or_more_shards,
 *  ]
 */

import {
    getRandomShardName,
    setupTestDatabase
} from "jstests/libs/sharded_cluster_fixture_helpers.js";
import {getUUIDFromConfigCollections} from "jstests/libs/uuid_util.js";

const fromCollName = 'from';
const toCollName = 'to';

/**
 * Initialize a sharded collection with key 'x:1' and 2 chunks distributed on 2 different nodes -
 * each containing 1 document.
 */
function setupShardedCollection(conn, dbName, collName) {
    const testDB = conn.getSiblingDB(dbName);
    const ns = dbName + '.' + collName;
    const primaryShardId = conn.getSiblingDB(dbName).getDatabasePrimaryShardId();
    const nonPrimaryShardId = getRandomShardName(db, /* exclude = */[primaryShardId]);
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
    assert.neq(chunk0.shard, chunk1.shard, 'Chunks expected to be on different shards');

    const toColl = testDB.getCollection(toCollName);
    assert.eq(toColl.countDocuments({x: 0}), 1, 'Expected exactly one document on the shard');
    assert.eq(toColl.countDocuments({x: 2}), 1, 'Expected exactly one document on the shard');
}

// Successful rename must pass tags from source to the target collection
{
    const dbName = 'testRenameFromTaggedCollection';
    const testDB = setupTestDatabase(db, dbName);

    const fromNs = dbName + '.' + fromCollName;
    const toNs = dbName + '.' + toCollName;

    const primaryShardId = testDB.getDatabasePrimaryShardId();
    const nonPrimaryShardId = getRandomShardName(db, [primaryShardId]);

    assert.commandWorked(testDB.adminCommand({addShardToZone: primaryShardId, zone: 'x'}));
    assert.commandWorked(testDB.adminCommand({addShardToZone: nonPrimaryShardId, zone: 'y'}));
    assert.commandWorked(
        testDB.adminCommand({updateZoneKeyRange: fromNs, min: {x: 0}, max: {x: 2}, zone: 'x'}));
    assert.commandWorked(
        testDB.adminCommand({updateZoneKeyRange: fromNs, min: {x: 2}, max: {x: 4}, zone: 'y'}));
    assert.commandWorked(testDB.adminCommand({shardCollection: fromNs, key: {x: 1}}));

    var fromTags = testDB.getSiblingDB('config').tags.find({ns: fromNs}).toArray();

    const fromColl = testDB.getCollection(fromCollName);
    fromColl.insert({x: 1});

    assert.commandWorked(fromColl.renameCollection(toCollName, false /* dropTarget */));

    const toTags = testDB.getSiblingDB('config').tags.find({ns: toNs}).toArray();
    assert.eq(toTags.length, 2, "Expected 2 tags associated to the target collection");

    function deleteDifferentTagFields(tag, index, array) {
        delete tag['_id'];
        delete tag['ns'];
    }
    fromTags.forEach(deleteDifferentTagFields);
    toTags.forEach(deleteDifferentTagFields);

    // Compare field by field because keys can potentially be in different order
    assert.docEq(fromTags[0], toTags[0], "Expected source tags to be passed to target collection");
    assert.docEq(fromTags[1], toTags[1], "Expected source tags to be passed to target collection");

    fromTags = testDB.getSiblingDB('config').tags.find({ns: fromNs}).toArray();
    assert.eq(fromTags.length, 0, "Expected no tags associated to the source collection");
}

// Rename to target collection with tags must fail
{
    const dbName = 'testRenameToTaggedCollection';
    const testDB = setupTestDatabase(db, dbName);

    const fromNs = dbName + '.' + fromCollName;
    const toNs = dbName + '.' + toCollName;
    assert.commandWorked(testDB.adminCommand({addShardToZone: getRandomShardName(db), zone: 'x'}));
    assert.commandWorked(
        testDB.adminCommand({updateZoneKeyRange: toNs, min: {x: 0}, max: {x: 10}, zone: 'x'}));

    assert.commandWorked(testDB.adminCommand({shardCollection: fromNs, key: {x: 1}}));

    const fromColl = testDB.getCollection(fromCollName);
    fromColl.insert({x: 1});
    assert.commandFailed(fromColl.renameCollection(toCollName, false /* dropTarget*/));
}

// Rename to target collection with very a long name
{
    const dbName = 'testRenameToCollectionWithVeryLongName';
    const testDB = setupTestDatabase(db, dbName);

    setupShardedCollection(testDB, dbName, fromCollName);
    const longEnoughCollName = 'x'.repeat(235 - `${dbName}.`.length);

    testRename(testDB,
               dbName,
               fromCollName,
               longEnoughCollName,
               false /* dropTarget */,
               false /* mustFail */);

    const tooLongCollName = longEnoughCollName + 'x';

    testRename(testDB,
               dbName,
               longEnoughCollName,
               tooLongCollName,
               false /* dropTarget */,
               true /* mustFail */);
}
