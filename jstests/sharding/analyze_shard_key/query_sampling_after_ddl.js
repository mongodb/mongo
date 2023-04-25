/**
 * Tests that query sampling stops when the collection is dropped or renamed.
 *
 * @tags: [requires_fcv_71]
 */

(function() {
"use strict";

load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

function setUpCollection(conn, {isShardedColl, st}) {
    const dbName = "testDb-" + extractUUIDFromObject(UUID());
    const collName = isShardedColl ? "testCollSharded" : "testCollUnsharded";
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);

    assert.commandWorked(db.createCollection(collName));
    if (isShardedColl) {
        assert(st);
        assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
        st.ensurePrimaryShard(dbName, st.shard0.name);
        assert.commandWorked(st.s0.adminCommand({shardCollection: ns, key: {x: 1}}));
        assert.commandWorked(st.s0.adminCommand({split: ns, middle: {x: 0}}));
        assert.commandWorked(
            st.s0.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));
    }

    return {dbName, collName};
}

function enableQuerySampling(conn, dbName, collName, {rst, st}) {
    const ns = dbName + "." + collName;
    const collUuid = QuerySamplingUtil.getCollectionUuid(conn.getDB(dbName), collName);
    assert.commandWorked(
        conn.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate: 1000}));
    QuerySamplingUtil.waitForActiveSampling(ns, collUuid, {rst, st});
}

function assertNumSampledQueries(conn, ns, expectedNum) {
    // Wait for one refresh interval so that if 'expectedNum' is 0 the check doesn't pass just
    // because the sampled queries have been flushed.
    sleep(queryAnalysisWriterIntervalSecs * 1000);

    let sampledQueryDocs;
    assert.soon(() => {
        const aggRes = assert.commandWorked(conn.adminCommand(
            {aggregate: 1, pipeline: [{$listSampledQueries: {namespace: ns}}], cursor: {}}));
        sampledQueryDocs = aggRes.cursor.firstBatch;
        if (sampledQueryDocs.length >= expectedNum) {
            return true;
        }
        return false;
    });
    assert.eq(sampledQueryDocs.length, expectedNum, sampledQueryDocs);
}

function testDropCollection(conn, {recreateCollection, isShardedColl, rst, st}) {
    assert(rst || st);
    assert(!rst || !st);

    const {dbName, collName} = setUpCollection(conn, {isShardedColl, st});
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    jsTest.log(
        `Testing dropCollection ${tojson({dbName, collName, isShardedColl, recreateCollection})}`);

    enableQuerySampling(conn, dbName, collName, {rst, st});

    assert(db.getCollection(collName).drop());
    if (recreateCollection) {
        assert.commandWorked(db.createCollection(collName));
    }
    // Verify that no queries get sampled.
    assert.commandWorked(db.runCommand({find: collName, filter: {x: 0}}));
    assertNumSampledQueries(conn, ns, 0);

    if (recreateCollection) {
        // Re-enable query sampling and verify that queries get sampled.
        enableQuerySampling(conn, dbName, collName, {rst, st});
        assert.commandWorked(db.runCommand({find: collName, filter: {x: 0}}));
        assertNumSampledQueries(conn, ns, 1);
    }
}

function testDropDatabase(conn, {recreateCollection, isShardedColl, rst, st}) {
    assert(rst || st);
    assert(!rst || !st);

    const {dbName, collName} = setUpCollection(conn, {isShardedColl, st});
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    jsTest.log(`Testing testDropDatabase ${
        tojson({dbName, collName, isShardedColl, recreateCollection})}`);

    enableQuerySampling(conn, dbName, collName, {rst, st});

    assert.commandWorked(db.dropDatabase());
    if (recreateCollection) {
        assert.commandWorked(db.createCollection(collName));
    }
    // Verify that no queries get sampled.
    assert.commandWorked(db.runCommand({find: collName, filter: {x: 0}}));
    assertNumSampledQueries(conn, ns, 0);

    if (recreateCollection) {
        // Re-enable query sampling and verify that queries get sampled.
        enableQuerySampling(conn, dbName, collName, {rst, st});
        assert.commandWorked(db.runCommand({find: collName, filter: {x: 0}}));
        assertNumSampledQueries(conn, ns, 1);
    }
}

function testRenameCollection(conn, {sameDatabase, isShardedColl, rst, st}) {
    assert(rst || st);
    assert(!rst || !st);

    const {dbName, collName} = setUpCollection(conn, {isShardedColl, st});

    const srcDbName = dbName;
    const srcCollName = collName;
    const srcNs = srcDbName + "." + srcCollName;
    const srcDb = conn.getDB(srcDbName);

    const dstDbName = sameDatabase ? srcDbName : (srcDbName + "New");
    const dstCollName = sameDatabase ? (srcCollName + "New") : srcCollName;
    const dstNs = dstDbName + "." + dstCollName;
    const dstDb = conn.getDB(dstDbName);
    assert.commandWorked(dstDb.createCollection(dstCollName));
    if (!sameDatabase && st) {
        // On a sharded cluster, the src and dst collections must be on same shard.
        st.ensurePrimaryShard(dstDbName, st.getPrimaryShardIdForDatabase(srcDbName));
    }

    jsTest.log(`Testing configuration deletion upon renameCollection ${
        tojson({sameDatabase, srcDbName, srcCollName, dstDbName, dstCollName, isShardedColl})}`);

    enableQuerySampling(conn, srcDbName, srcCollName, {rst, st});
    enableQuerySampling(conn, dstDbName, dstCollName, {rst, st});

    assert.commandWorked(conn.adminCommand({renameCollection: srcNs, to: dstNs, dropTarget: true}));
    // Verify that no queries get sampled for the src and dst collections.
    assert.commandWorked(srcDb.runCommand({find: srcCollName, filter: {x: 0}}));
    assertNumSampledQueries(conn, srcNs, 0);
    assert.commandWorked(dstDb.runCommand({find: dstCollName, filter: {x: 0}}));
    assertNumSampledQueries(conn, dstNs, 0);

    // Enable query sampling for the new collection and verify that queries get sampled.
    enableQuerySampling(conn, dstDbName, dstCollName, {rst, st});
    assert.commandWorked(conn.getDB(dstDbName).runCommand({find: dstCollName, filter: {x: 0}}));
    assertNumSampledQueries(conn, dstNs, 1);
}

const queryAnalysisSamplerConfigurationRefreshSecs = 1;
const queryAnalysisWriterIntervalSecs = 1;

const mongodSetParameterOpts = {
    queryAnalysisSamplerConfigurationRefreshSecs,
    queryAnalysisWriterIntervalSecs,
    logComponentVerbosity: tojson({sharding: 2}),
};
const mongosSetParametersOpts = {
    queryAnalysisSamplerConfigurationRefreshSecs,
    logComponentVerbosity: tojson({sharding: 3})
};

{
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 1, setParameter: mongodSetParameterOpts},
        mongosOptions: {setParameter: mongosSetParametersOpts}
    });

    for (let isShardedColl of [true, false]) {
        for (let recreateCollection of [true, false]) {
            testDropCollection(st.s, {st, recreateCollection, isShardedColl});
            testDropDatabase(st.s, {st, recreateCollection, isShardedColl});
        }
        testRenameCollection(st.s, {st, sameDatabase: true, isShardedColl});
    }
    // The source database is only allowed to be different from the destination database when the
    // collection being renamed is unsharded.
    testRenameCollection(st.s, {st, sameDatabase: false, isShardedColl: false});

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: mongodSetParameterOpts}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    for (let recreateCollection of [true, false]) {
        testDropCollection(primary, {rst, recreateCollection});
        testDropDatabase(primary, {rst, recreateCollection});
    }
    for (let sameDatabase of [true, false]) {
        testRenameCollection(primary, {rst, sameDatabase});
    }

    rst.stopSet();
}
})();
