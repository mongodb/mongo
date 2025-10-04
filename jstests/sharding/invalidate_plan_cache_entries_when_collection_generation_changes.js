/**
 * Tests that plan cache entries are deleted after shard key refining, resharding and renaming
 * operations.
 *
 *  @tags: [
 *   # The SBE plan cache was enabled by default in 6.3.
 *   requires_fcv_63,
 *   featureFlagSbeFull,
 *   # TODO (SERVER-85629): Re-enable this test once redness is resolved in multiversion suites.
 *   DISABLED_TEMPORARILY_DUE_TO_FCV_UPGRADE,
 *   requires_fcv_80
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const criticalSectionTimeoutMS = 24 * 60 * 60 * 1000; // 1 day
const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 1,
    other: {
        // Avoid spurious failures with small 'ReshardingCriticalSectionTimeout' values being set.
        configOptions: {setParameter: {reshardingCriticalSectionTimeoutMillis: criticalSectionTimeoutMS}},
    },
});

const mongos = st.s;
const dbName = "invalidate_on_coll_generation_change_db";
const db = st.getDB(dbName);
const collA = db["collA"];
const collB = db["collB"];

function assertPlanCacheSizeForColl(nss, expectedEntriesCount) {
    // Using assert.soon since the sharded metadata cleanup function is executed asynchronously.
    assert.soon(() => {
        const entries = mongos
            .getCollection(nss)
            .aggregate([{$planCacheStats: {}}])
            .toArray();
        let numSBEEntries = 0;
        entries.forEach((entry) => {
            if (entry.version == "2") numSBEEntries++;
        });
        jsTestLog(
            "99999 actual SBE entries: " +
                numSBEEntries +
                ", expected SBE entries: " +
                expectedEntriesCount +
                ", total entries: " +
                entries.length,
        );

        return numSBEEntries === expectedEntriesCount;
    });
}

function assertDropAndShardColl(coll, keyDoc) {
    assert.commandWorked(db.runCommand({drop: coll.getName()}));
    assert.commandWorked(mongos.adminCommand({shardCollection: coll.getFullName(), key: keyDoc}));
}

// Initializes the collection and makes sure there's exactly one plan cache entry after
// initialization.
function initCollection(nss) {
    assertPlanCacheSizeForColl(nss, 0);

    assert.commandWorked(mongos.getCollection(nss).insert({a: 1, b: 2, aKey: 1}));
    assert.commandWorked(mongos.getCollection(nss).insert({a: 2, b: 2, aKey: 2}));

    assert.commandWorked(mongos.getCollection(nss).createIndex({a: 1}));
    assert.commandWorked(mongos.getCollection(nss).createIndex({b: 1}));
    assert.commandWorked(mongos.getCollection(nss).createIndex({a: 1, b: 1}));

    // Run query multiple times to activate a plan cache entry.
    assert.eq(mongos.getCollection(nss).find({a: 1, b: 1}).itcount(), 0);
    assert.eq(mongos.getCollection(nss).find({a: 1, b: 1}).itcount(), 0);
    assertPlanCacheSizeForColl(nss, 1);
}

assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));

// Test that plan cache entries are deleted after refining shard key of a collection.
(function testRefineShardKeyDeletesAssociatedCacheEntry() {
    jsTestLog("Testing refine shard key command");
    // Set up the collections.
    for (let coll of [collA, collB]) {
        assertDropAndShardColl(coll, {a: 1});
        initCollection(coll.getFullName());
    }

    // Ensure that after refining the shard key there are no plan cache entries associated with the
    // 'collA'. However, plan cache entries for 'collB' must remain unchanged.
    assert.commandWorked(mongos.adminCommand({refineCollectionShardKey: collA.getFullName(), key: {a: 1, b: 1}}));

    // The refine shard key command may complete but shards might not be aware of it.
    st.shard0.adminCommand({_flushRoutingTableCacheUpdates: collA.getFullName(), syncFromConfig: true});

    assertPlanCacheSizeForColl(collA.getFullName(), 0);
    assertPlanCacheSizeForColl(collB.getFullName(), 1);
})();

// Test that plan cache entries are deleted after collection is resharded.
(function testReshardingDeletesAssociatedCacheEntry() {
    jsTestLog("Testing reshard collection command");
    // Set up the collections.
    for (let coll of [collA, collB]) {
        assertDropAndShardColl(coll, {a: 1});
        initCollection(coll.getFullName());
    }

    // Ensure that after resharding there are no plan cache entries associated with the 'collA'.
    // However, plan cache entries for 'collB' must remain unchanged.
    assert.commandWorked(
        mongos.adminCommand({reshardCollection: collA.getFullName(), key: {b: 1}, numInitialChunks: 1}),
    );
    assertPlanCacheSizeForColl(collA.getFullName(), 0);
    assertPlanCacheSizeForColl(collB.getFullName(), 1);
})();

// Test that plan cache entries are deleted after a collection is renamed.
(function testRenameCollectionDeletesAssociatedCacheEntry() {
    jsTestLog("Testing rename collection command");
    // Set up the collections.
    for (let coll of [collA, collB]) {
        assertDropAndShardColl(coll, {a: 1});
        initCollection(coll.getFullName());
    }

    // Ensure that after renaming there are no plan cache entries associated with either 'collA' or
    // 'collB'.
    assert.commandWorked(
        mongos.adminCommand({renameCollection: collA.getFullName(), to: collB.getFullName(), dropTarget: true}),
    );

    assertPlanCacheSizeForColl(collB.getFullName(), 0);

    // 'collB' was dropped by the renameCollection cmd. Check that $planCacheStats returns the
    // exected error (i.e. collection doesn't exist)
    assert.commandFailedWithCode(
        db.runCommand({aggregate: collA.getName(), pipeline: [{$planCacheStats: {}}], cursor: {}}),
        50933,
    );
})();

st.stop();
