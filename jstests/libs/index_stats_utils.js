/**
 * Helpers for asserting on serverStatus().indexStats.
 */

/**
 * Fetch indexStats from serverStatus and pass them to `assertFn`. If the assertion fails, log the
 * full stats object for debugging before re-throwing.
 */
export const assertStats = (db, assertFn) => {
    const stats = db.serverStatus().indexStats;
    try {
        assertFn(stats);
    } catch (e) {
        print("indexStats result: " + tojson(stats));
        throw e;
    }
};

/**
 * Assert that all feature counts and the total count are zero.
 */
export const assertZeroCounts = (db) => {
    assertStats(db, (featureStats) => {
        assert.eq(featureStats.count, 0);
        for (const [feature, stats] of Object.entries(featureStats.features)) {
            assert.eq(0, stats.count, feature);
        }
    });
};

/**
 * Assert that all feature access counters are zero.
 */
export const assertZeroAccess = (db) => {
    assertStats(db, (featureStats) => {
        for (const [feature, stats] of Object.entries(featureStats.features)) {
            assert.eq(0, stats.accesses, feature);
        }
    });
};

/**
 * Assert that `current.count` equals `last.count + inc`.
 */
export const assertCountIncrease = (last, current, inc) => {
    assert.eq(last.count + inc, current.count, "incorrect index count");
};

/**
 * Assert that a specific feature's count increased by `inc` between two snapshots.
 */
export const assertFeatureCountIncrease = (last, current, feature, inc) => {
    assert.eq(
        last.features[feature].count + inc,
        current.features[feature].count,
        "incorrect feature count for " + feature,
    );
};

/**
 * Assert that a specific feature's accesses increased by `inc` between two snapshots.
 */
export const assertFeatureAccessIncrease = (last, current, feature, inc) => {
    assert.eq(
        last.features[feature].accesses + inc,
        current.features[feature].accesses,
        "incorrect feature accesses for " + feature,
    );
};
