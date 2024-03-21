/*
 * Tests that the replica set endpoint skips readPreference re-targeting.
 *
 * @tags: [
 *   requires_fcv_73,
 *   featureFlagEmbeddedRouter,
 * ]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {waitForAutoBootstrap} from "jstests/noPassthrough/rs_endpoint/lib/util.js";

const rst = new ReplSetTest({
    nodes: 3,
    nodeOptions: {
        setParameter: {
            featureFlagAllMongodsAreSharded: true,
            featureFlagReplicaSetEndpoint: true,
            // TODO (SERVER-84243): When the feature flag below is enabled, the collections in this
            // test would be tracked so a read on a secondary may trigger a catalog cache refresh.
            // The router service on mongod currently uses the ShardServerCatalogCacheLoader.
            // As a result, a refresh on a secondary with replica set endpoint enabled currently
            // involves running _flushDatabaseCacheUpdates on the primary and then waiting for the
            // refreshed metadata to get replicated. This test later pauses replication on one of
            // the secondaries. So the refresh on that secondary is expected to hang.
            featureFlagTrackUnshardedCollectionsUponCreation: false,
            logComponentVerbosity: tojson({sharding: 2}),
        }
    },
    // Disallow chaining to force both secondaries to sync from the primary. This test later
    // pauses replication on one of the secondaries, with chaining that would effectively pause
    // replication on both secondaries and cause the test to to hang since writeConcern
    // {w: "majority"} would be unsatsifiable.
    settings: {chainingAllowed: false},
    useAutoBootstrapProcedure: true,
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondaries = rst.getSecondaries();

waitForAutoBootstrap(primary);

// Disable the balancer since this test later pauses replication on one of the secondaries
// and each balancer round involves refreshing the shard registry which is a read against the
// config.shards collection with readPreference "nearest" so it may target the paused secondary
// and then get stuck waiting for afterClusterTime. For an explanation for why the refresh might
// target paused secondary, see SERVER-81910.
assert.commandWorked(primary.adminCommand({balancerStop: 1}));
assert.soon(() => {
    const res = assert.commandWorked(primary.adminCommand({balancerStatus: 1}));
    return !res.inBalancerRound;
})

const dbName = "testDb";
const collName = "testColl";

const primaryTestDB = primary.getDB(dbName);
const secondary0TestDB = secondaries[0].getDB(dbName);
const secondary1TestDB = secondaries[1].getDB(dbName);

const primaryTestColl = primaryTestDB.getCollection(collName);
const secondary0TestColl = secondary0TestDB.getCollection(collName);
const secondary1TestColl = secondary1TestDB.getCollection(collName);

const primaryProfilerColl = primaryTestDB.getCollection("system.profile");
const secondary0ProfilerColl = secondary0TestDB.getCollection("system.profile");
const secondary1ProfilerColl = secondary1TestDB.getCollection("system.profile");

{
    // Perform a write and wait for it to replicate to all secondaries.
    assert.commandWorked(primaryTestColl.insert({x: 1}, {writeConcern: {w: 3}}));

    const primaryDocs = primaryTestColl.find().sort({x: 1}).toArray();
    const secondary0Docs = secondary0TestColl.find({}).sort({x: 1}).toArray();
    const secondary1Docs = secondary1TestColl.find({}).sort({x: 1}).toArray();
    assert.eq(primaryDocs.length, 1, primaryDocs);
    assert.eq(primaryDocs[0].x, 1, primaryDocs);
    assert.sameMembers(primaryDocs, secondary0Docs);
    assert.sameMembers(secondary0Docs, secondary1Docs);
}

jsTest.log("Enabling profiler");
assert.commandWorked(primaryTestDB.setProfilingLevel(2));
assert.commandWorked(secondary0TestDB.setProfilingLevel(2));
assert.commandWorked(secondary1TestDB.setProfilingLevel(2));

{
    jsTest.log("Testing that the replica set endpoint doesn't re-target reads when the " +
               "readPreference is not 'primary'");
    const readPreferences = ["secondary", "primaryPreferred", "secondaryPreferred", "nearest"];

    // Stop replication on secondary1.
    stopServerReplication(secondaries[1]);
    // Perform a write and wait for it to replicate to secondary0.
    assert.commandWorked(primaryTestColl.insert({x: 2}, {writeConcern: {w: "majority"}}));
    jsTest.log("Primary's clusterTime after insert " + tojson(primary.getClusterTime()));

    for (let readPreference of readPreferences) {
        jsTest.log(`Reading directly from primary but specifying readPreference ${readPreference}`);
        const primaryComment = extractUUIDFromObject(UUID());
        const primaryDocs = primaryTestColl.find()
                                .sort({x: 1})
                                .comment(primaryComment)
                                .readPref(readPreference)
                                .toArray();

        jsTest.log(
            `Reading directly from secondary0 but specifying readPreference ${readPreference}`);
        const secondary0Comment = extractUUIDFromObject(UUID());
        const secondary0Docs = secondary0TestColl.find({})
                                   .sort({x: 1})
                                   .comment(secondary0Comment)
                                   .readPref(readPreference)
                                   .toArray();

        jsTest.log(
            `Reading directly from secondary1 but specifying readPreference ${readPreference}`);
        const secondary1Comment = extractUUIDFromObject(UUID());
        const secondary1Docs = secondary1TestColl.find({})
                                   .sort({x: 1})
                                   .comment(secondary1Comment)
                                   .readPref(readPreference)
                                   .toArray();
        jsTest.log("Find results " + tojson({primaryDocs, secondary0Docs, secondary1Docs}));

        assert.eq(primaryDocs.length, 2, primaryDocs);
        assert.sameMembers(primaryDocs, secondary0Docs);
        assert.eq(primaryDocs[0].x, 1, primaryDocs);
        assert.eq(primaryDocs[1].x, 2, primaryDocs);
        // The read against secondary1 shouldn't see the newly inserted doc.
        assert.eq(secondary1Docs.length, 1, secondary1Docs);
        assert.eq(secondary1Docs[0].x, 1, secondary1Docs);

        const primaryProfilerFilter = {"command.comment": primaryComment};
        assert.neq(primaryProfilerColl.findOne(primaryProfilerFilter), null);
        assert.eq(secondary0ProfilerColl.findOne(primaryProfilerFilter), null);
        assert.eq(secondary1ProfilerColl.findOne(primaryProfilerFilter), null);

        const secondary0ProfilerFilter = {"command.comment": secondary0Comment};
        assert.eq(primaryProfilerColl.findOne(secondary0ProfilerFilter), null);
        assert.neq(secondary0ProfilerColl.findOne(secondary0ProfilerFilter), null);
        assert.eq(secondary1ProfilerColl.findOne(secondary0ProfilerFilter), null);

        const secondary1ProfilerFilter = {"command.comment": secondary1Comment};
        assert.eq(primaryProfilerColl.findOne(secondary1ProfilerFilter), null);
        assert.eq(secondary0ProfilerColl.findOne(secondary1ProfilerFilter), null);
        assert.neq(secondary1ProfilerColl.findOne(secondary1ProfilerFilter), null);
    }

    restartServerReplication(secondaries[1]);
}

{
    jsTest.log("Testing that the replica set endpoint does not re-target reads when the " +
               "readPreference is 'primary'");

    const comment = extractUUIDFromObject(UUID());
    assert.throwsWithCode(
        () => secondary0TestColl.find().sort({x: 1}).comment(comment).readPref("primary").toArray(),
        ErrorCodes.NotPrimaryNoSecondaryOk);

    const profilerFilter = {"command.comment": comment};
    assert.eq(primaryProfilerColl.findOne(profilerFilter), null);
    assert.eq(secondary0ProfilerColl.findOne(profilerFilter), null);
    assert.eq(secondary1ProfilerColl.findOne(profilerFilter), null);
}

{
    jsTest.log("Testing that the replica set endpoint can handle writes from $out or " +
               "$merge targeted to a secondary");

    const outComment = extractUUIDFromObject(UUID());
    const outCollName = collName + "_out";
    secondary0TestColl
        .aggregate(
            [
                {$match: {x: 1}},
                {$out: outCollName},
            ],
            {comment: outComment})
        .toArray();
    assert.neq(primaryTestDB.getCollection(outCollName).findOne({x: 1}), null);

    const outProfilerFilter = {"command.aggregate": collName, "command.comment": outComment};
    assert.eq(primaryProfilerColl.findOne(outProfilerFilter), null);
    assert.neq(secondary0ProfilerColl.findOne(outProfilerFilter), null);
    assert.eq(secondary1ProfilerColl.findOne(outProfilerFilter), null);

    const mergeComment = extractUUIDFromObject(UUID());
    const mergeCollName = collName + "_merge";
    secondary1TestColl
        .aggregate(
            [
                {$match: {x: 1}},
                {$merge: {into: mergeCollName}},
            ],
            {comment: mergeComment})
        .toArray();
    assert.neq(primaryTestDB.getCollection(mergeCollName).findOne({x: 1}), null);

    const mergeProfilerFilter = {"command.aggregate": collName, "command.comment": mergeComment};
    assert.eq(primaryProfilerColl.findOne(mergeProfilerFilter), null);
    assert.eq(secondary0ProfilerColl.findOne(mergeProfilerFilter), null);
    assert.neq(secondary1ProfilerColl.findOne(mergeProfilerFilter), null);
}

{
    jsTest.log("Testing that the replica set endpoint does not re-target writes");

    assert.commandFailedWithCode(secondary0TestColl.insert({x: 3}), ErrorCodes.NotWritablePrimary);
    assert.eq(primaryTestColl.findOne({x: 3}), null);

    assert.commandFailedWithCode(secondary1TestColl.update({x: 1}, {$set: {y: 1}}),
                                 ErrorCodes.NotWritablePrimary);
    assert.eq(primaryTestColl.findOne({x: 1}).y, null);

    assert.commandFailedWithCode(secondary0TestColl.remove({x: 1}), ErrorCodes.NotWritablePrimary);
    assert.neq(primaryTestColl.findOne({x: 1}), null);

    assert.throwsWithCode(
        () => secondary1TestColl.findAndModify({query: {x: 1}, update: {$set: {y: 1}}}),
        ErrorCodes.NotWritablePrimary);
    assert.eq(primaryTestColl.findOne({x: 1}).y, null);

    assert.commandFailedWithCode(secondary0TestColl.createIndex({x: 1}),
                                 ErrorCodes.NotWritablePrimary);
    // The collection should only have the _id index.
    assert.eq(primaryTestColl.getIndexes().length, 1);

    assert.commandWorked(primaryTestColl.createIndex({x: 1}));
    assert.eq(primaryTestColl.getIndexes().length, 2);
    assert.commandFailedWithCode(secondary1TestColl.dropIndex({x: 1}),
                                 ErrorCodes.NotWritablePrimary);
    assert.eq(primaryTestColl.getIndexes().length, 2);

    assert.throwsWithCode(() => secondary0TestColl.drop(), ErrorCodes.NotWritablePrimary);
    assert.eq(primaryTestDB.getCollectionInfos({name: collName}).length, 1);

    const newCollName = collName + "Other";
    assert.commandFailedWithCode(secondary1TestDB.createCollection(newCollName),
                                 ErrorCodes.NotWritablePrimary);
    assert.eq(primaryTestDB.getCollectionInfos({name: newCollName}).length, 0);

    assert.commandFailedWithCode(secondary0TestColl.renameCollection(newCollName),
                                 ErrorCodes.NotWritablePrimary);
    assert.eq(primaryTestDB.getCollectionInfos({name: collName}).length, 1);
    assert.eq(primaryTestDB.getCollectionInfos({name: newCollName}).length, 0);
}

jsTest.log("Disabling profiler");
assert.commandWorked(primaryTestDB.setProfilingLevel(0));
assert.commandWorked(secondary0TestDB.setProfilingLevel(0));
assert.commandWorked(secondary1TestDB.setProfilingLevel(0));

// TODO (SERVER-83433): Make the replica set have secondaries to get test coverage
// for running db hash check while the replica set is fsync locked.
rst.remove(1);
rst.remove(1);
MongoRunner.stopMongod(secondaries[0]);
MongoRunner.stopMongod(secondaries[1]);

rst.stopSet();
