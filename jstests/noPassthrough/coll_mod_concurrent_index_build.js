/**
 * Regression test for SERVER-107819. Tests running a collMod operation while an index build is
 * in progress in a non-DB-primary shard.
 */

(function() {
'use strict';

load('jstests/libs/parallelTester.js');                           // For Thread
load('jstests/noPassthrough/libs/index_build.js');                // For IndexBuildTest
load("jstests/sharding/libs/create_sharded_collection_util.js");  // For CreateShardedCollectionUtil

const st = new ShardingTest({shards: 2, config: 1});
const db = st.s.getDB("test");
assert.commandWorked(
    st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

function runCollModWithConcurrentIndexBuild(coll, collModOptions, {permitIndexBuildAbort} = {}) {
    // Force a two-phase index build to hang on the non DB primary shard.
    const shardConn = st.rs1.getPrimary();
    IndexBuildTest.pauseIndexBuilds(shardConn);
    const awaitIndexBuild = IndexBuildTest.startIndexBuild(
        st.s,
        coll.getFullName(),
        {a: 1},
        {} /* options */,
        permitIndexBuildAbort ? [ErrorCodes.IndexBuildAborted] : [],
    );
    const underlyingIndexBuildColl = (function() {
        const bucketsColl = coll.getDB().getCollection("system.buckets." + coll.getName());
        return bucketsColl.exists() ? bucketsColl : coll;
    })();
    IndexBuildTest.waitForIndexBuildToScanCollection(
        shardConn.getDB(db.getName()), underlyingIndexBuildColl.getName(), "a_1");

    // Use collMod to change the granularity on the collection.
    const collModThread = new Thread(
        function(host, dbName, collName, collModOptions) {
            const db = new Mongo(host).getDB(dbName);
            assert.commandWorked(db.runCommand({collMod: collName, ...collModOptions}));
        },
        db.getMongo().host,
        db.getName(),
        coll.getName(),
        collModOptions,
    );
    collModThread.start();

    // Wait some time for the collMod to likely run into the index build.
    sleep(1000);

    // Let everything finish and assert collection options are still consistent.
    IndexBuildTest.resumeIndexBuilds(shardConn);

    collModThread.join();
    awaitIndexBuild();

    const inconsistencies = coll.getDB().checkMetadataConsistency().toArray();
    assert.eq(0, inconsistencies.length, tojson(inconsistencies));

    // Manual check since CollectionOptionsMismatch is not fully supported on MongoDB v7.0
    // This takes advantage of $listCatalog returning data for non-owning shards (SERVER-64980).
    const listCatalog =
        db.getSiblingDB("admin")
            .aggregate([
                {$listCatalog: {}},
                {$match: {db: db.getName(), name: underlyingIndexBuildColl.getName()}},
            ])
            .toArray();
    for (const entry of listCatalog) {
        assert.docEq(entry.md.options, listCatalog[0].md.options, tojson(listCatalog));
    }
}

{
    // Sharded collection, all data outside DB primary.
    const coll = db.getCollection("sharded_outside_db_primary");
    CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
        {min: {x: MinKey}, max: {x: MaxKey}, shard: st.shard1.shardName},
    ]);
    coll.insertOne({x: 123});
    runCollModWithConcurrentIndexBuild(coll, {validator: {x: 123}});
}

{
    // Regular sharded collection, data on both shards.
    const coll = db.getCollection("sharded");
    CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
        {min: {x: MinKey}, max: {x: 0}, shard: st.shard1.shardName},
        {min: {x: 0}, max: {x: MaxKey}, shard: st.shard1.shardName},
    ]);
    coll.insertMany([{x: -321}, {x: 123}]);
    runCollModWithConcurrentIndexBuild(coll, {validator: {x: 123}});
}

{
    // Timeseries collection sharded on meta, data only on non-primary shard.
    const coll = db.getCollection("ts_sharded_on_meta");
    assert.commandWorked(
        st.s.adminCommand({
            shardCollection: coll.getFullName(),
            key: {m: 1},
            timeseries: {timeField: "t", metaField: "m"},
        }),
    );
    assert.commandWorked(
        st.s.adminCommand({
            moveChunk: `${db.getName()}.system.buckets.${coll.getName()}`,
            find: {"meta": 0},
            to: st.shard1.shardName,
        }),
    );
    coll.insertOne({t: ISODate(), m: 123, temp: 42});
    // Since timeseries granularity changes block CRUD operations, the index build may be aborted to
    // unblock them.
    runCollModWithConcurrentIndexBuild(
        coll, {timeseries: {granularity: "minutes"}}, {permitIndexBuildAbort: true});
}

{
    // Timeseries collection sharded on time, data only on non-primary shard.
    const coll = db.getCollection("ts_sharded_on_time");
    assert.commandWorked(
        st.s.adminCommand({
            shardCollection: coll.getFullName(),
            key: {t: 1},
            timeseries: {timeField: "t"},
        }),
    );
    assert.commandWorked(
        st.s.adminCommand({
            moveChunk: `${db.getName()}.system.buckets.${coll.getName()}`,
            find: {"control.min.t": 0},
            to: st.shard1.shardName,
        }),
    );
    coll.insertOne({t: ISODate(), temp: 42});
    // Since timeseries granularity changes block CRUD operations, the index build may be aborted to
    // unblock them.
    runCollModWithConcurrentIndexBuild(
        coll, {timeseries: {granularity: "minutes"}}, {permitIndexBuildAbort: true});
}

st.stop();
})();
