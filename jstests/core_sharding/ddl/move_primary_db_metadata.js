/*
 * @tags: [
 *  # movePrimary command is not allowed in clusters with a single shard.
 *  requires_2_or_more_shards,
 *  # movePrimary fails if there is a stepdown during data cloning phase.
 *  does_not_support_stepdowns,
 * ]
 */

import {getRandomShardName} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

function getDbMetadata(db) {
    return db.getSiblingDB('config').databases.findOne({_id: db.getName()});
}

// TODO remove once SERVER-98049 is fixed.
db.dropDatabase();

const originalPrimary = getRandomShardName(db);
assert.commandWorked(
    db.adminCommand({enableSharding: db.getName(), primaryShard: originalPrimary}));

const originalMetadata = getDbMetadata(db);
assert.eq(originalPrimary, originalMetadata.primary);
assert.eq(originalPrimary, db.getDatabasePrimaryShardId());

{
    // move to same shard must not change metadata
    assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: originalPrimary}));

    const newMetadata = getDbMetadata(db);
    assert.docEq(newMetadata, originalMetadata);
}

{
    const newPrimary = getRandomShardName(db, /*exclude=*/ originalPrimary);
    assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: newPrimary}));

    const newMetadata = getDbMetadata(db);

    assert.eq(newPrimary, newMetadata.primary);
    assert.eq(newPrimary, db.getDatabasePrimaryShardId());

    // UUID has not changed, but timestamp and lastMod have been bumped.
    assert.eq(originalMetadata.version.uuid, newMetadata.version.uuid);
    assert.eq(-1, timestampCmp(originalMetadata.version.timestamp, newMetadata.version.timestamp));
    assert.eq(originalMetadata.version.lastMod + 1, newMetadata.version.lastMod);
}
