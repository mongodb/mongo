/*
 * Test the $listClusterCatalog stage in a sharded cluster for any type of collection,
  collection tracking state and stage's specs.
 *
 *
 * @tags: [
 *    # TODO (SERVER-98651) remove the tag as part of this ticket.
 *   requires_fcv_81,
 *   # Requires to know the exact list of shards owning chunks for the collection.
 *   # Requires exact knowledge on whether the collection is tracked or not.
 *   assumes_balancer_off
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// nss => { shardinginfo: {}, shards: []}
let cachedCatalog = {};

function ns(dbName, collName) {
    return dbName + "." + collName;
}

function nsBucket(dbName, collName) {
    return dbName + ".system.buckets." + collName;
}

function isSharded(nss) {
    if (!cachedCatalog[nss].shardinginfo)
        return false;
    return cachedCatalog[nss].shardinginfo.unsplittable !== true;
}

function isTracked(nss) {
    if (!cachedCatalog[nss].shardinginfo)
        return false;
    return true;
}

function getShardKey(nss) {
    if (!isSharded(nss))
        return undefined;
    return cachedCatalog[nss].shardinginfo.key;
}

function getShardList(nss) {
    return cachedCatalog[nss].shards;
}

function getBalancerEnabled(nss) {
    if (!isSharded(nss))
        return undefined;
    // noBalance is false by default.
    return (cachedCatalog[nss].shardinginfo.noBalance !== true);
}

function getAutoMergingEnabled(nss) {
    if (!isSharded(nss))
        return undefined;
    // autoMerger is true by default.
    return (cachedCatalog[nss].shardinginfo.enableAutoMerge !== false);
}

function getStageResultForNss(stageResult, nss) {
    return stageResult.find((obj) => {
        return obj.ns === nss;
    });
}

function getChunkSize(nss) {
    if (!isSharded(nss)) {
        return undefined;
    }

    if (cachedCatalog[nss].shardinginfo.maxChunkSizeBytes) {
        return cachedCatalog[nss].shardinginfo.maxChunkSizeBytes / (1024 * 1024);
    } else {
        return cachedCatalog.defaultChunkSize;
    }
}

// Test all the combination of specs. Every spec can be true or false. The total number of
// combinations will be 2^n where n is number of specs.
function generateSpecCombinations(specs) {
    const totalCombinations = Math.pow(2, specs.length);
    const combinations = [];

    for (let i = 0; i < totalCombinations; i++) {
        const combination = {};
        for (let j = 0; j < specs.length; j++) {
            combination[specs[j]] = Boolean(i & (1 << j));  // 0 for false, a value for true.
        }

        combinations.push(combination);
    }
    return combinations;
}

function verify(expectedResult, result) {
    // Sorts arrays to simplify comparisons.
    if (expectedResult.shards) {
        expectedResult.shards.sort();
    }
    if (result.shards) {
        result.shards.sort();
    }

    // Ensure the resuls fields matches the expected once.
    assert.eq(expectedResult.options, result.options, "options field mismatch:" + tojson(result));
    assert.eq(expectedResult.info, result.info, "info field mismatch:" + tojson(result));
    assert.eq(expectedResult.idIndex, result.idIndex, "idIndex field mismatch:" + tojson(result));
    assert.eq(expectedResult.type, result.type, "type field mismatch:" + tojson(result));
    assert.eq(expectedResult.sharded, result.sharded, "sharded field mismatch:" + tojson(result));
    assert.eq(expectedResult.db, result.db, "db field mismatch:" + tojson(result));
    assert.eq(expectedResult.ns, result.ns, "ns field mismatch:" + tojson(result));

    // Ensure this field is no longer present.
    assert.eq(undefined, result.name, "name field mismatch:" + tojson(result));

    assert.eq(expectedResult.shards, result.shards, "shards field mismatch:" + tojson(result));
    assert.eq(
        expectedResult.shardKey, result.shardKey, "shardKey field mismatch:" + tojson(result));
    assert.eq(expectedResult.tracked, result.tracked, "tracked field mismatch:" + tojson(result));
    assert.eq(expectedResult.balancingEnabled,
              result.balancingEnabled,
              "balancingEnabled field mismatch:" + tojson(result));
    assert.eq(expectedResult.autoMergingEnabled,
              result.autoMergingEnabled,
              "autoMergingEnabled field mismatch:" + tojson(result));
    assert.eq(
        expectedResult.chunkSize, result.chunkSize, "chunkSize field mismatch:" + tojson(result));
}

function verifyAgainstListCollections(listCollectionResult, stageResult, specs) {
    assert.eq(listCollectionResult.length,
              stageResult.length,
              "Unxpected number of namespaces reported by the stage");

    listCollectionResult
        .forEach(
            (expectedResult) => {
                let nss = ns(expectedResult.dbName, expectedResult.name);
                let nssStageResult = getStageResultForNss(stageResult, nss);
                // The result must be present.
                assert.neq(nssStageResult,
            undefined,
            `The namespace ${
                nss} was found in listCollections but not in the $listClusterCatalog. Result ${
                tojson(stageResult)}`);
                // The result must match the entire list collection result + cluster
                // information.
                expectedResult.db = expectedResult.dbName;
                expectedResult.ns = nss;
                expectedResult.sharded = isSharded(nss);
                expectedResult.shardKey = getShardKey(nss);
                if (specs.tracked) {
                    expectedResult.tracked = isTracked(nss);
                } else {
                    expectedResult.tracked = undefined;
                }
                if (specs.shards) {
                    expectedResult.shards = getShardList(nss);
                } else {
                    expectedResult.shards = undefined;
                }
                if (specs.balancingConfiguration) {
                    expectedResult.balancingEnabled = getBalancerEnabled(nss);
                    expectedResult.autoMergingEnabled = getAutoMergingEnabled(nss);
                    expectedResult.chunkSize = getChunkSize(nss);
                } else {
                    expectedResult.balancingEnabled = undefined;
                    expectedResult.autoMergingEnabled = undefined;
                    expectedResult.chunkSize = undefined;
                }

                verify(expectedResult, nssStageResult);
            });
}

function cacheCatalog(conn, dbName, collName, primaryShard, shards, isTimeseries, isView) {
    // store information for system.views
    if (isTimeseries || isView) {
        let systemviewNs = ns(dbName, "system.views");
        cachedCatalog[systemviewNs] = {};
        cachedCatalog[systemviewNs].shards = [primaryShard];
    }

    // store information for the view.
    if (isTimeseries) {
        let viewNs = ns(dbName, collName);
        cachedCatalog[viewNs] = {};
        cachedCatalog[viewNs].shards = [primaryShard];
    }

    // store informations for the target nss in the sharding catalog.
    // Note for unsharded collection shardinginfo will be undefined.
    let targetNs = isTimeseries ? nsBucket(dbName, collName) : ns(dbName, collName);
    cachedCatalog[targetNs] = {};
    cachedCatalog[targetNs].shardinginfo =
        conn.getDB("config").getCollection("collections").find({_id: targetNs}).toArray()[0];
    cachedCatalog[targetNs].shards = shards;
}

function setupUnshardedCollections(conn, dbName, primaryShard) {
    const isTrackUnshardedUponCreationEnabled = FeatureFlagUtil.isPresentAndEnabled(
        conn.getDB('admin'), "TrackUnshardedCollectionsUponCreation");

    conn.adminCommand({enablesharding: dbName, primaryShard: primaryShard});

    const kViewCollName = "view";
    const kTimeseriesCollName = "timeseries";
    let collList = ["coll1", "coll2", kTimeseriesCollName, kViewCollName];

    // Do not test unsharded collection in case of TrackUnshardedCollectionsUponCreation or any
    // unsharded collection is otherwise tracked. An equivalent test is performed later. Test only
    // the view.
    if (isTrackUnshardedUponCreationEnabled) {
        collList = [kViewCollName];
    }
    // Create all the collections
    collList.forEach((collName) => {
        let nss = ns(dbName, collName);
        if (collName == kTimeseriesCollName) {
            assert.commandWorked(conn.getDB(dbName).createCollection(
                collName, {timeseries: {metaField: "meta", timeField: "timestamp"}}));
        } else if (collName == kViewCollName) {
            assert.commandWorked(
                conn.getDB(dbName).createCollection(collName, {viewOn: "coll1", pipeline: []}));
        } else {
            conn.getDB(dbName).createCollection(collName);
        }

        cacheCatalog(conn,
                     dbName,
                     collName,
                     primaryShard,
                     [primaryShard],
                     collName == kTimeseriesCollName,
                     collName == kViewCollName);

        // Verify the helpers will report the correct information.
        assert.eq(isSharded(nss), false);
        assert.eq(isTracked(nss), false);
        assert.eq(getAutoMergingEnabled(nss), undefined);
        assert.eq(getBalancerEnabled(nss), undefined);
        assert.eq(getShardKey(nss), undefined);
        assert.eq(getShardList(nss), [primaryShard]);
        assert.eq(getChunkSize(nss), undefined);

        if (collName == kTimeseriesCollName) {
            let bucketNs = nsBucket(dbName, collName);
            assert.eq(isSharded(bucketNs), false);
            assert.eq(isTracked(bucketNs), false);
            assert.eq(getAutoMergingEnabled(bucketNs), undefined);
            assert.eq(getBalancerEnabled(bucketNs), undefined);
            assert.eq(getShardKey(bucketNs), undefined);
            assert.eq(getShardList(bucketNs), [primaryShard]);
            assert.eq(getChunkSize(bucketNs), undefined);
        }
        if (collName == kViewCollName) {
            let systemviewNs = ns(dbName, "system.views");
            assert.eq(isSharded(systemviewNs), false);
            assert.eq(isTracked(systemviewNs), false);
            assert.eq(getAutoMergingEnabled(systemviewNs), undefined);
            assert.eq(getBalancerEnabled(systemviewNs), undefined);
            assert.eq(getShardKey(systemviewNs), undefined);
            assert.eq(getShardList(systemviewNs), [primaryShard]);
            assert.eq(getChunkSize(systemviewNs), undefined);
        }
    });
}

function setupShardedCollections(st, dbName, primaryShard) {
    const mongos = st.s;
    mongos.adminCommand({enablesharding: dbName, primaryShard: primaryShard});

    const kUnbalancedColl = "unbalanced";
    const kNoAutoMerger = 'nomerger';
    const kTimeseriesCollName = "timeseries";
    const kDifferentChunkSize = "differentchunksize";
    const collList = [
        "coll1",
        "coll2",
        kTimeseriesCollName,
        kUnbalancedColl,
        kNoAutoMerger,
        kDifferentChunkSize
    ];

    // Create all the collections
    collList.forEach((collName) => {
        let nss = ns(dbName, collName);
        let shardKey = {x: "hashed"};
        let shardList = [st.shard0.shardName, st.shard1.shardName, st.shard2.shardName];
        let chunkSize = cachedCatalog.defaultChunkSize;
        if (collName == kTimeseriesCollName) {
            shardKey = {meta: 1};
            shardList = [primaryShard];
            assert.commandWorked(mongos.getDB(dbName).createCollection(
                collName, {timeseries: {metaField: "meta", timeField: "timestamp"}}));
        }

        assert.commandWorked(
            mongos.adminCommand({shardCollection: nss, key: shardKey, unique: false}));

        if (collName == kUnbalancedColl) {
            // Disable balancing for the collection
            assert.commandWorked(
                mongos.getCollection(ns(dbName, kUnbalancedColl)).disableBalancing());
        }

        if (collName == kNoAutoMerger) {
            // Disable merger for the collection
            assert.commandWorked(mongos.adminCommand({
                configureCollectionBalancing: ns(dbName, kNoAutoMerger),
                enableAutoMerger: false
            }));
        }

        if (collName == kDifferentChunkSize) {
            chunkSize = 64;
            // Change default chunk size for the collection
            assert.commandWorked(mongos.adminCommand({
                configureCollectionBalancing: ns(dbName, kDifferentChunkSize),
                chunkSize: chunkSize
            }));
        }

        const isTimeseries = collName == kTimeseriesCollName;
        cacheCatalog(st, dbName, collName, primaryShard, shardList, isTimeseries, false);

        // Verify the helpers will report the correct information.
        let targetNs = isTimeseries ? nsBucket(dbName, collName) : ns(dbName, collName);
        assert.eq(isSharded(targetNs), true);
        assert.eq(isTracked(targetNs), true);
        assert.eq(getAutoMergingEnabled(targetNs), collName != kNoAutoMerger);
        assert.eq(getBalancerEnabled(targetNs), collName != kUnbalancedColl);
        assert.eq(getShardKey(targetNs), shardKey);
        assert.eq(getShardList(targetNs).sort(), shardList.sort());
        assert.eq(getChunkSize(targetNs), chunkSize);
    });
}

function setupTrackedUnshardedCollections(st, dbName, primaryShard, toShard) {
    const mongos = st.s;
    mongos.adminCommand({enablesharding: dbName, primaryShard: primaryShard});

    const kUnbalancedColl = "unbalanced";
    const kNoAutoMerger = 'nomerger';
    const kDifferentChunkSize = "differentchunksize";
    const collList = ["coll1", "coll2", kUnbalancedColl, kNoAutoMerger, kDifferentChunkSize];
    collList.forEach(collName => {
        let nss = ns(dbName, collName);
        st.s.getDB(dbName).createCollection(collName);
        assert.commandWorked(mongos.adminCommand({moveCollection: nss, toShard: toShard}));

        if (collName == kUnbalancedColl) {
            // Disable balancing for the collection
            assert.commandWorked(
                mongos.getCollection(ns(dbName, kUnbalancedColl)).disableBalancing());
        }

        if (collName == kNoAutoMerger) {
            // Disable merger for the collection
            assert.commandWorked(mongos.adminCommand({
                configureCollectionBalancing: ns(dbName, kNoAutoMerger),
                enableAutoMerger: false
            }));
        }

        if (collName == kDifferentChunkSize) {
            // Change default chunk size for the collection
            assert.commandWorked(mongos.adminCommand(
                {configureCollectionBalancing: ns(dbName, kDifferentChunkSize), chunkSize: 64}));
        }

        cacheCatalog(st, dbName, collName, primaryShard, [toShard], false, false);

        // Verify the helpers will report the correct information.
        assert.eq(isSharded(nss), false);
        assert.eq(isTracked(nss), true);
        assert.eq(getAutoMergingEnabled(nss), undefined);
        assert.eq(getBalancerEnabled(nss), undefined);
        assert.eq(getShardKey(nss), undefined);
        assert.eq(getShardList(nss), [toShard]);
        assert.eq(getChunkSize(nss), undefined);
    });
}

function runListCollectionsOnDbs(conn, dbNames) {
    let result = [];
    dbNames.forEach((dbName) => {
        let dbResult = conn.getDB(dbName).runCommand({listCollections: 1}).cursor.firstBatch;
        dbResult.forEach((collectionInfo) => {
            // Attach the dbName as extra information to calculate the full ns during the
            // verification step.
            collectionInfo.dbName = dbName;
            result.push(collectionInfo);
        });
    });

    return result;
}

const kNotExistent = 'notExistent';
const kUnshardedDB = "unshardedDB";
const kShardedDB = "shardedDB";
const kTrackedUnshardedDB = "trackedUnshardedDB";
const kSpecsList = ["shards", "tracked", "balancingConfiguration"];

const st = new ShardingTest({shards: 3});
const mongos = st.s;

const kPrimaryShard = st.shard0.shardName;
const kToShard = st.shard1.shardName;

cachedCatalog.defaultChunkSize = 128;

jsTest.log("The stage must run collectionless.");
{
    assert.commandFailedWithCode(
        mongos.getDB("test").runCommand(
            {aggregate: "foo", pipeline: [{$listClusterCatalog: {}}], cursor: {}}),
        9621301);
}

jsTest.log("The stage must return an empty result if the cluster has no user collections.");
{
    let result = assert
                     .commandWorked(mongos.getDB("admin").runCommand(
                         {aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}))
                     .cursor.firstBatch;
    assert.eq(0, result.length, result);
}

jsTest.log("The stage must return the collection for the specified user db. Case unsharded.");
{
    setupUnshardedCollections(mongos, kUnshardedDB, kPrimaryShard);

    let listCollectionResult = runListCollectionsOnDbs(mongos, [kUnshardedDB]);

    let stageResult =
        assert
            .commandWorked(
                mongos.getDB(kUnshardedDB)
                    .runCommand({aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}))
            .cursor.firstBatch;
    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log("The stage must return the collection for the specified user db. Case sharded.");
{
    setupShardedCollections(st, kShardedDB, kPrimaryShard);

    let listCollectionResult = runListCollectionsOnDbs(mongos, [kShardedDB]);

    let stageResult =
        assert
            .commandWorked(
                mongos.getDB(kShardedDB)
                    .runCommand({aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}))
            .cursor.firstBatch;
    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log(
    "The stage must return the collection for the specified user db. Case tracked unsharded.");
{
    setupTrackedUnshardedCollections(st, kTrackedUnshardedDB, kPrimaryShard, kToShard);

    let listCollectionResult = runListCollectionsOnDbs(mongos, [kTrackedUnshardedDB]);

    let stageResult =
        assert
            .commandWorked(
                mongos.getDB(kTrackedUnshardedDB)
                    .runCommand({aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}))
            .cursor.firstBatch;
    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log("The stage must return an empty result if the user database doesn't exist.");
{
    let result =
        assert
            .commandWorked(
                mongos.getDB(kNotExistent)
                    .runCommand({aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}))
            .cursor.firstBatch;
    assert.eq(0, result.length, result);
}

jsTest.log("The stage must return every database if run against the admin db.");
{
    let listCollectionResult =
        runListCollectionsOnDbs(mongos, [kUnshardedDB, kShardedDB, kTrackedUnshardedDB]);

    let stageResult = assert
                          .commandWorked(mongos.getDB("admin").runCommand(
                              {aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}}))
                          .cursor.firstBatch;
    verifyAgainstListCollections(listCollectionResult, stageResult, {});
}

jsTest.log("The stage must work under any combination of specs.");
{
    let listCollectionResult =
        runListCollectionsOnDbs(mongos, [kUnshardedDB, kShardedDB, kTrackedUnshardedDB]);

    // Test all the combination of specs.
    let allSpecs = generateSpecCombinations(kSpecsList);
    allSpecs.forEach((specs) => {
        jsTest.log("Verify the stage reports the correct result for specs " + tojson(specs));
        let stageResult =
            assert
                .commandWorked(mongos.getDB("admin").runCommand(
                    {aggregate: 1, pipeline: [{$listClusterCatalog: specs}], cursor: {}}))
                .cursor.firstBatch;

        verifyAgainstListCollections(listCollectionResult, stageResult, specs);
    });
}

jsTest.log("The stage must report the correct default chunk size if changed.");
{
    // Updating the default chunk size
    const newSizeMb = 32;
    st.config.settings.updateOne(
        {_id: "chunksize"}, {$set: {_id: "chunksize", value: newSizeMb}}, {upsert: true});
    cachedCatalog.defaultChunkSize = newSizeMb;

    let listCollectionResult =
        runListCollectionsOnDbs(mongos, [kUnshardedDB, kShardedDB, kTrackedUnshardedDB]);

    const spec = {balancingConfiguration: true};
    let stageResult = assert
                          .commandWorked(mongos.getDB("admin").runCommand(
                              {aggregate: 1, pipeline: [{$listClusterCatalog: spec}], cursor: {}}))
                          .cursor.firstBatch;

    verifyAgainstListCollections(listCollectionResult, stageResult, spec);
}
st.stop();
