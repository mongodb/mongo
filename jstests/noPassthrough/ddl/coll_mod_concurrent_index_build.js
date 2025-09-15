/**
 * Regression test for SERVER-107819. Tests running a collMod operation while an index build is
 * in progress in a non-DB-primary shard.
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const st = new ShardingTest({shards: 2, config: 1});
const db = st.s.getDB("test");
assert.commandWorked(st.s.adminCommand({enableSharding: db.getName(), primaryShard: st.shard0.shardName}));

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
    IndexBuildTest.waitForIndexBuildToScanCollection(shardConn.getDB(db.getName()), coll.getName(), "a_1");

    // Use collMod to change the granularity on the collection.
    const collModThread = new Thread(
        function (host, dbName, collName, collModOptions) {
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
}

{
    // Regular unsharded collection outside DB primary.
    const coll = db.getCollection("unsharded");
    coll.insertOne({x: 123});
    assert.commandWorked(st.s.adminCommand({moveCollection: coll.getFullName(), toShard: st.shard1.shardName}));
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
    CreateShardedCollectionUtil.shardCollectionWithChunks(
        coll,
        {m: 1},
        [{min: {meta: MinKey}, max: {meta: MaxKey}, shard: st.shard1.shardName}],
        {timeseries: {timeField: "t", metaField: "m"}},
    );
    coll.insertOne({t: ISODate(), m: 123, temp: 42});
    // Since timeseries granularity changes block CRUD operations, the index build may be aborted to unblock them.
    runCollModWithConcurrentIndexBuild(coll, {timeseries: {granularity: "minutes"}}, {permitIndexBuildAbort: true});
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
            moveChunk: getTimeseriesCollForDDLOps(db, coll).getFullName(),
            find: {"control.min.t": 0},
            to: st.shard1.shardName,
        }),
    );
    coll.insertOne({t: ISODate(), temp: 42});
    // Since timeseries granularity changes block CRUD operations, the index build may be aborted to unblock them.
    runCollModWithConcurrentIndexBuild(coll, {timeseries: {granularity: "minutes"}}, {permitIndexBuildAbort: true});
}

st.stop();
