/*
 * Basic test for move primary.
 *
 * @tags: [
 *  # movePrimary command is not allowed in clusters with a single shard.
 *  requires_2_or_more_shards,
 *  # TODO SERVER-98125 re-enable this test in config_shard_incompatible suites.
 *  # Suites with config shard uses "implicitly_retry_on_shard_transition_errors" override library.
 *  # This library keep retrying if movePrimary fails with ShardNotFound.
 *  # This test expects some movePrimary calls to actually fail with ShardNotFound.
 *  config_shard_incompatible,
 *  # movePrimary fails if there is a stepdown during data cloning phase.
 *  does_not_support_stepdowns,
 * ]
 */

import {getRandomShardName} from 'jstests/libs/sharded_cluster_fixture_helpers.js';

{
    jsTest.log('Fail with missing destination shard');

    assert.commandFailed(db.adminCommand({movePrimary: db.getName()}));
}

{
    jsTest.log('Fail with invalid destination shard');

    const invalidShardNames = ['', 'unknown', 'nonExistingShard', 'shard1000'];

    // Move primary with non existing shard must fail even if the database does not exists
    invalidShardNames.forEach(invalidShardName => {
        assert.commandFailed(db.adminCommand({movePrimary: db.getName(), to: invalidShardName}));
    });

    // Move primary with non existing shard must fail when the database exists
    db.coll.insertOne({a: 1});
    invalidShardNames.forEach(invalidShardName => {
        assert.commandFailed(db.adminCommand({movePrimary: db.getName(), to: invalidShardName}));
    });
    assert.eq(1, db.coll.countDocuments({}));
    db.dropDatabase();
}

{
    jsTest.log('Fail with internal databases');
    const internalDBs = ['config', 'admin', 'local'];

    const validShardId = getRandomShardName(db);
    internalDBs.forEach(dbName => {
        assert.commandFailed(db.adminCommand({movePrimary: dbName, to: validShardId}),
                             `movePrimary succeeded on internal database ${dbName}`);
        assert.commandFailed(
            db.adminCommand({movePrimary: dbName, to: 'nonExistingShard'}),
            `movePrimary succeeded on internal database ${dbName} and non existing shard`);
        assert.commandFailed(
            db.adminCommand({movePrimary: dbName, to: 'config'}),
            `movePrimary succeeded on internal database ${dbName} and non existing 'config' shard`);
    });
}

{
    jsTest.log("Fail with invalid db name");

    const invalidDbNames = ['', 'a.b'];

    const validShardId = getRandomShardName(db);
    invalidDbNames.forEach(dbName => {
        assert.commandFailed(db.adminCommand({movePrimary: dbName, to: validShardId}),
                             `movePrimary succeeded on invalid database name '${dbName}'`);
    });
}

{
    // Fail against a non-admin database.
    assert.commandFailedWithCode(
        db.runCommand({movePrimary: db.getName(), to: getRandomShardName(db)}),
        ErrorCodes.Unauthorized);
}

{
    // Succeed if the destination shard is already the primary for the given database.
    assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
    const primaryShardId = db.getDatabasePrimaryShardId();
    assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: primaryShardId}));
    assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: primaryShardId}));
}
