/**
 * Tests using the collectionUUID parameter when operating on a time-series collection.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   assumes_stable_collection_uuid,
 *   featureFlagTimeseriesUpdatesSupport,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function retryableUpdateTestUUIDMismatch(isSharded) {
    const dbName = jsTestName();
    const collName = "coll";
    const timeField = "time";
    const metaField = "m";
    const nonexistentUUID = UUID();

    let primary;
    let testDB;
    let session;
    let cluster;
    let sDB;

    if (isSharded) {
        cluster = new ShardingTest({shards: 2});
        sDB = cluster.s.getDB(dbName);
        assert.commandWorked(sDB.adminCommand({enableSharding: dbName, primaryShard: cluster.shard0.shardName}));
        primary = cluster.rs0.getPrimary();
    } else {
        cluster = new ReplSetTest({nodes: 2});
        cluster.startSet();
        cluster.initiate();
        primary = cluster.getPrimary();
    }

    session = primary.startSession({retryWrites: true});
    testDB = session.getDatabase(dbName);

    assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: timeField, metaField: metaField}}));

    if (isSharded) {
        assert.commandWorked(
            sDB.adminCommand({
                shardCollection: `${dbName}.${collName}`,
                key: {[metaField]: 1},
            }),
        );
    }

    let res = testDB.runCommand(
        session._serverSession.assignTransactionNumber({
            update: collName,
            updates: [
                {
                    q: {m: 1},
                    u: {$set: {m: 1}},
                },
            ],
            collectionUUID: testDB[collName].getUUID(),
        }),
    );

    jsTest.log(`Result: ${tojson(res)}`);
    assert.commandWorked(res);

    res = testDB.runCommand(
        session._serverSession.assignTransactionNumber({
            update: collName,
            updates: [
                {
                    q: {m: 1},
                    u: {$set: {m: 1}},
                },
            ],
            collectionUUID: nonexistentUUID,
        }),
    );

    jsTest.log(`Result: ${tojson(res)}`);
    assert.commandFailedWithCode(res, ErrorCodes.CollectionUUIDMismatch);

    if (isSharded) {
        cluster.stop();
    } else {
        cluster.stopSet();
    }
}

retryableUpdateTestUUIDMismatch(/* isSharded */ false);
retryableUpdateTestUUIDMismatch(/* isSharded */ true);
