/**
 * Tests that trying to move a collection to the shard it currently
 * exists on is a noop (which can be done by confirming the collection's UUID
 * remains unchanged after the operation).
 *
 * @tags: [
 *   uses_atclustertime,
 *   requires_fcv_72,
 *   featureFlagReshardingImprovements,
 *   featureFlagMoveCollection,
 *   featureFlagTrackUnshardedCollectionsUponCreation,
 *   multiversion_incompatible,
 *   assumes_balancer_off
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";

const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 2},
});

const dbName = 'db';
const unsplittableCollName = "foo_unsplittable"
const ns = dbName + '.' + unsplittableCollName;
let shard0 = st.shard0.shardName;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard0}));
assert.commandWorked(
    st.s.getDB(dbName).runCommand({createUnsplittableCollection: unsplittableCollName}));

const sourceCollection = st.s.getCollection(ns);
const mongos = sourceCollection.getMongo();
const sourceDB = sourceCollection.getDB();

// The UUID should remain the same if the toShard is the shard that the collection currently exists
// on.
const preMoveCollectionUUID = getUUIDFromListCollections(sourceDB, sourceCollection.getName());
assert.commandWorked(mongos.adminCommand({moveCollection: ns, toShard: shard0}));
const postMoveCollectionUUID = getUUIDFromListCollections(sourceDB, sourceCollection.getName());
assert.eq(preMoveCollectionUUID, postMoveCollectionUUID);

st.stop();
