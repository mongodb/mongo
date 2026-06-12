/**
 * Tests that the reshardCollection command aborts correctly when the transaction for updating
 * the persistent state (e.g. config.collections and config.tags) in the resharding commit phase
 * fails with a TransactionTooLargeForCache error, for timeseries collections. Verifies that
 * collection metadata (config.collections, config.chunks, config.tags) is unchanged after abort.
 *
 * Timeseries variant of resharding_update_tag_zones_large.js.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {getTimeseriesCollForDDLOps} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

function assertEqualObj(lhs, rhs, keysToIgnore) {
    assert.eq(Object.keys(lhs).length, Object.keys(lhs).length, {lhs, rhs});
    for (let key in rhs) {
        if (keysToIgnore && keysToIgnore.has(key)) {
            continue;
        }

        const value = rhs[key];
        if (typeof value === "object") {
            assertEqualObj(lhs[key], rhs[key], keysToIgnore);
        } else {
            assert.eq(lhs[key], rhs[key], {key, actual: lhs, expected: rhs});
        }
    }
}

const st = new ShardingTest({
    shards: 2,
    // Disable the cluster parameter refresher to prevent it from interfering with the fail point
    // used to force a TransactionTooLargeForCache error on commitTransaction.
    mongosOptions: {setParameter: {"failpoint.skipClusterParameterRefresh": "{'mode':'alwaysOn'}"}},
    configOptions: {
        setParameter: {"reshardingCriticalSectionTimeoutMillis": 24 * 60 * 60 * 1000 /* 1 day */},
    },
});
const configRSPrimary = st.configRS.getPrimary();

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

const configDB = st.s.getDB("config");
const collectionsColl = configDB.getCollection("collections");
const chunksColl = configDB.getCollection("chunks");
const tagsColl = configDB.getCollection("tags");

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);

// Create a timeseries collection with a non-"meta" metaField name to exercise the shard key
// translation path (user-facing field -> internal bucket field).
assert.commandWorked(
    st.s.adminCommand({
        shardCollection: ns,
        key: {"sensorId.x": 1},
        timeseries: {timeField: "ts", metaField: "sensorId"},
    }),
);

const zoneName = "testZone";
assert.commandWorked(st.s.adminCommand({addShardToZone: st.shard0.shardName, zone: zoneName}));

// config.collections and config.tags store the bucket namespace for viewful timeseries (FCV < 9.0)
// and the user-facing namespace for viewless timeseries (FCV >= 9.0).
const configNs = getTimeseriesCollForDDLOps(
    st.s.getDB(dbName),
    st.s.getDB(dbName).getCollection(collName),
).getFullName();

// Set up an initial zone range using configNs so that the zone is stored under the same namespace
// that config.collections and config.tags use. This ensures the tagsBefore lookup below finds it
// regardless of whether the mongos translates updateZoneKeyRange namespaces (old binary) or not
// (new binary at FCV < 9.0).
assert.commandWorked(
    st.s.adminCommand({
        updateZoneKeyRange: configNs,
        min: {"meta.x": MinKey},
        max: {"meta.x": 0},
        zone: zoneName,
    }),
);

const collBefore = collectionsColl.findOne({_id: configNs});
assert.neq(collBefore, null);
const chunksBefore = chunksColl.find({uuid: collBefore.uuid}).sort({lastmod: -1}).toArray();
assert.gte(chunksBefore.length, 1, chunksBefore);
const tagsBefore = tagsColl.find({ns: configNs}).toArray();
assert.gte(tagsBefore.length, 1, tagsBefore);

const reshardingFunc = (mongosHost, ns, zoneName) => {
    const mongos = new Mongo(mongosHost);
    jsTest.log("Start resharding");
    const reshardingRes = mongos.adminCommand({
        reshardCollection: ns,
        key: {"sensorId.y": 1},
        unique: false,
        collation: {locale: "simple"},
        zones: [{zone: zoneName, min: {"meta.y": MinKey}, max: {"meta.y": 0}}],
        numInitialChunks: 2,
    });
    jsTest.log("Finished resharding");
    return reshardingRes;
};
let reshardingThread = new Thread(reshardingFunc, st.s.host, ns, zoneName);

const persistFp = configureFailPoint(
    configRSPrimary,
    "reshardingPauseCoordinatorBeforeDecisionPersisted",
);
reshardingThread.start();
persistFp.wait();

const commitFp = configureFailPoint(
    configRSPrimary,
    "failCommand",
    {
        failCommands: ["commitTransaction"],
        failInternalCommands: true,
        failLocalClients: true,
        errorCode: ErrorCodes.TransactionTooLargeForCache,
    },
    {times: 1},
);
persistFp.off();
commitFp.wait();
commitFp.off();
const reshardingRes = reshardingThread.returnData();

assert.commandFailedWithCode(reshardingRes, ErrorCodes.TransactionTooLargeForCache);

const collAfter = collectionsColl.findOne({_id: configNs});
assert.neq(collAfter, null);
const chunksAfter = chunksColl.find({uuid: collAfter.uuid}).sort({lastmod: -1}).toArray();
const tagsAfter = tagsColl.find({ns: configNs}).toArray();

jsTest.log(
    "Verify that the collection metadata remains the same since the resharding operation failed.",
);

assertEqualObj(collBefore, collAfter);

assert.eq(chunksBefore.length, chunksAfter.length, {chunksBefore, chunksAfter});
for (let i = 0; i < chunksAfter.length; i++) {
    // Ignore "lastmod" when verifying the newest chunk because resharding bumps the minor version
    // of the newest chunk whenever it goes through a state transition.
    assertEqualObj(chunksBefore[i], chunksAfter[i], new Set(i == 0 ? ["lastmod"] : []));
}

assert.eq(tagsBefore.length, tagsAfter.length, {tagsBefore, tagsAfter});
for (let i = 0; i < tagsAfter.length; i++) {
    assertEqualObj(tagsBefore[i], tagsAfter[i]);
}

st.stop();
