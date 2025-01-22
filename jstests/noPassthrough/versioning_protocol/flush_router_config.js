/*
 * Test covering the basic functionality of the flushRouterConfig command sent to a mongos process
 * and its observable effects through serverStatus.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({});

const statsFields = ['numDatabaseEntries', 'numCollectionEntries'];

function getCatalogCacheStats() {
    return assert.commandWorked(
        st.s.adminCommand({serverStatus: 1}))['shardingStatistics']['catalogCache'];
}

function buildStatsDeltas(dbEntries, collEntries) {
    return {numDatabaseEntries: dbEntries, numCollectionEntries: collEntries};
}

function getLatestStatsAndCheckAgainst(previousStats, expectedDeltas) {
    const latestStats = getCatalogCacheStats();
    for (let stat of statsFields) {
        assert.eq(expectedDeltas[stat],
                  latestStats[stat] - previousStats[stat],
                  `Unexpected delta for stat field ${stat}: current values are ${
                      tojson(latestStats)}, previous ones are ${tojson(previousStats)}`);
    }
    return latestStats;
}

function doQueryTargetingNamespace(nss) {
    const [dbName, collName] = nss.split('.');
    assert.eq(0, st.s.getDB(dbName)[collName].countDocuments({}));
}

const dbName1 = 'db1';
const collName11 = 'coll1';
const collName12 = 'coll2';
const nss11 = `${dbName1}.${collName11}`;
const nss12 = `${dbName1}.${collName12}`;

const dbName2 = 'db2';
const collName21 = 'coll1';
const nss21 = `${dbName2}.${collName21}`;

const testDbNames = [dbName1, dbName2];
const fullCollectionNames = [nss11, nss12, nss21];

// Test setup: before acquiring an initial snapshot of catalog cache stats, ensure that the cache
// metadata about config.system.sessions are stable.

assert.commandWorked(st.s.adminCommand({shardCollection: 'config.system.sessions', key: {_id: 1}}));

const initialCacheStats = getCatalogCacheStats();

// Use the available mongos connection to create the namespaces and perform a first read on each one
// of them; counters about cache misses and consecutive fetched entries should be incremented
// accordingly.
for (let nss of fullCollectionNames) {
    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: "hashed"}}));
    doQueryTargetingNamespace(nss);
}

const cacheStatsUponNssCreation = getLatestStatsAndCheckAgainst(
    initialCacheStats,
    buildStatsDeltas(testDbNames.length /*dbEntries*/, fullCollectionNames.length /*collEntries*/));

const cacheStatsUponShardVersionCheck = getLatestStatsAndCheckAgainst(
    cacheStatsUponNssCreation, buildStatsDeltas(0 /*dbEntries*/, 0 /*collEntries*/));

// A flushRouterConfig at collection level should only empty the collection cache.
jsTest.log("Testing flushRouterConfig at collection level...");
assert.commandWorked(st.s.adminCommand({flushRouterConfig: nss11}));

const cacheStatsUponCollectionMetadataFlush = getLatestStatsAndCheckAgainst(
    cacheStatsUponShardVersionCheck, buildStatsDeltas(0 /*dbEntries*/, -1 /*collEntries*/));

// A new command targeting the flushed cache entries should refill them.
doQueryTargetingNamespace(nss11);
const cacheStatsUponFlushedCollectionRefresh = getLatestStatsAndCheckAgainst(
    cacheStatsUponCollectionMetadataFlush, buildStatsDeltas(0 /*dbEntries*/, 1 /*collEntries*/));

// Accessing other namespaces should cause no effects.
doQueryTargetingNamespace(nss12);
doQueryTargetingNamespace(nss21);
getLatestStatsAndCheckAgainst(cacheStatsUponFlushedCollectionRefresh,
                              buildStatsDeltas(0 /*dbEntries*/, 0 /*collEntries*/));

jsTest.log("Testing flushRouterConfig at database level (containing 2 collections)...");
assert.commandWorked(st.s.adminCommand({flushRouterConfig: dbName1}));

// All the collections under the flushed dbName should be affected.
const cacheStatsUponDatabaseMetadataFlush = getLatestStatsAndCheckAgainst(
    cacheStatsUponFlushedCollectionRefresh, buildStatsDeltas(-1 /*dbEntries*/, -2 /*collEntries*/));

doQueryTargetingNamespace(nss11);
doQueryTargetingNamespace(nss12);
const cacheStatsUponFlushedDatabaseRefresh = getLatestStatsAndCheckAgainst(
    cacheStatsUponDatabaseMetadataFlush, buildStatsDeltas(1 /*dbEntries*/, 2 /*collEntries*/));

// Other namespaces should keep their cached metadata intact.
doQueryTargetingNamespace(nss21);
getLatestStatsAndCheckAgainst(cacheStatsUponFlushedDatabaseRefresh,
                              buildStatsDeltas(0 /*dbEntries*/, 0 /*collEntries*/));

jsTest.log("Testing flushRouterConfig at cluster level...");
assert.commandWorked(st.s.adminCommand({flushRouterConfig: 1}));
const cacheStatsUponFullFlush = getLatestStatsAndCheckAgainst(
    cacheStatsUponFlushedDatabaseRefresh,
    buildStatsDeltas(-cacheStatsUponNssCreation.numDatabaseEntries /*dbEntries*/,
                     -cacheStatsUponNssCreation.numCollectionEntries /*collEntries*/));

doQueryTargetingNamespace(nss11);
doQueryTargetingNamespace(nss12);
doQueryTargetingNamespace(nss21);

const _ = getLatestStatsAndCheckAgainst(
    cacheStatsUponFullFlush,
    buildStatsDeltas(testDbNames.length /*dbEntries*/, fullCollectionNames.length /*collEntries*/));

st.stop();
