/*
 * Checks that if the ConfigServerCatalogCacheLoader is in the middle of a refresh (it has read the
 * config.collections entry, but not config.chunks yet) when we change FCV, the cache is able to
 * refresh correctly.
 */

// @tags: [multiversion_incompatible]

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

let st = new ShardingTest({mongos: 2, shards: 2});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let csrs_config_db = st.configRS.getPrimary().getDB('config');
const isfeatureFlagShardingFullDDLSupportTimestampedVersionEnabled =
    csrs_config_db
        .adminCommand({getParameter: 1, featureFlagShardingFullDDLSupportTimestampedVersion: 1})
        .featureFlagShardingFullDDLSupportTimestampedVersion.value;

function refreshCatalogCacheWhileChangingFCV(newFCVVersion) {
    const numRefreshesBefore = st.s0.adminCommand({serverStatus: 1})
                                   .shardingStatistics.catalogCache.countFullRefreshesStarted;

    assert.commandWorked(st.s0.adminCommand({flushRouterConfig: ns}));
    const fp = configureFailPoint(st.s0, "hangBeforeReadingChunks");
    let awaitShell = startParallelShell(
        funWithArgs(function(dbName, collName) {
            assert.eq(1, db.getSiblingDB(dbName).getCollection(collName).find({x: 1}).itcount());
        }, dbName, collName), st.s0.port);

    fp.wait();
    assert.commandWorked(st.s1.adminCommand({setFeatureCompatibilityVersion: newFCVVersion}));
    // Ensure all config servers have replicated the patched-up metadata, so the catalog refresh
    // on s0 won't possibly pick a lagged secondary that hasn't replicated it yet.
    st.configRS.awaitLastOpCommitted();
    fp.off();
    awaitShell();

    const numRefreshesAfter = st.s0.adminCommand({serverStatus: 1})
                                  .shardingStatistics.catalogCache.countFullRefreshesStarted;

    if (isfeatureFlagShardingFullDDLSupportTimestampedVersionEnabled) {
        // TODO SERVER-53283 Remove once 5.0 has branched out.
        // Expect that the refresh had to be retried due to the ConfigServerCatalogCache loader
        // finding that the config.chunks format has changed since it had read the
        // config.collections earlier.
        assert.eq(2, numRefreshesAfter - numRefreshesBefore);
    } else {
        assert.eq(1, numRefreshesAfter - numRefreshesBefore);
    }
}

assert.commandWorked(st.s0.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s0.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s0.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s0.getDB(dbName).getCollection(collName).insert({x: 1}));

refreshCatalogCacheWhileChangingFCV(latestFCV);
refreshCatalogCacheWhileChangingFCV(lastLTSFCV);

st.stop();
})();
