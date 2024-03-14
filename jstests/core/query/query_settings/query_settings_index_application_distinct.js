// Tests query settings are applied to distinct queries regardless of the query engine (SBE or
// classic).
// @tags: [
//   # $planCacheStats can not be run with specified read preferences/concerns.
//   assumes_read_preference_unchanged,
//   assumes_read_concern_unchanged,
//   # $planCacheStats can not be run in transactions.
//   does_not_support_transactions,
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   cqf_incompatible,
//   # 'planCacheClear' command is not allowed with the security token.
//   not_allowed_with_signed_security_token,
//   requires_fcv_80,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsIndexHintsTests} from "jstests/libs/query_settings_index_hints_tests.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

// Create the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());
const qstests = new QuerySettingsIndexHintsTests(qsutils);
const ns = {
    db: db.getName(),
    coll: coll.getName()
};

// Ensure query settings are applied as expected in a straightforward scenario for distinct.
function assertQuerySettingsDistinctIndexApplication(querySettingsQuery) {
    const query = qsutils.withoutDollarDB(querySettingsQuery);
    for (const index of [qstests.indexA, qstests.indexAB]) {
        const settings = {indexHints: {ns, allowedIndexes: [index]}};
        qsutils.withQuerySettings(querySettingsQuery, settings, () => {
            // Avoid checking plan cache entries. Multiplanner is not involved into plans with
            // DISTINCT_SCAN stage thus one shouldn't expect new plan cache entries.
            qstests.assertDistinctScanStage(query, index);
        });
    }
}

// Ensure that the hint gets ignored when query settings for the particular 'distinct' query are
// set.
function assertQuerySettingsDistinctScanIgnoreCursorHints(querySettingsQuery) {
    const query = qsutils.withoutDollarDB(querySettingsQuery);
    const settings = {indexHints: {ns, allowedIndexes: [qstests.indexAB]}};
    const queryWithHint = {...query, hint: qstests.indexA};
    qsutils.withQuerySettings(querySettingsQuery, settings, () => {
        // Avoid checking plan cache entries. Multiplanner is not involved into plans with
        // DISTINCT_SCAN stage thus one shouldn't expect new plan cache entries.
        qstests.assertDistinctScanStage(queryWithHint, qstests.indexAB);
    });
}

// Ensure that distinct queries fall back to running distinct scan without query settings
// when
// the provided settings don't generate any viable plans.
function assertQuerySettingsDistinctFallback(querySettingsQuery) {
    // DISTINCT_SCAN has priority over query settings, i.e. if applying query settings falls
    // query planner back to IXSCAN or COLLSCAN but ignoring query settings would keep
    // DISTINCT_SCAN plan generated then query settings should be ignored.
    const query = qsutils.withoutDollarDB(querySettingsQuery);
    const settings = {indexHints: {ns, allowedIndexes: ["doesnotexist"]}};
    qsutils.withQuerySettings(querySettingsQuery, settings, () => {
        // Here any index is considered just as good as any other as long as DISTINCT_SCAN plan
        // is generated. Don't expect any particular index from the planner.
        const expectedIndex = undefined;
        qstests.assertDistinctScanStage(query, expectedIndex);
    });
}

// Insert data into the collection.
assert.commandWorked(coll.insertMany([
    {a: 1, b: 5},
    {a: 2, b: 4},
    {a: 3, b: 3},
    {a: 4, b: 2},
    {a: 5, b: 1},
]));

// Ensure that query settings cluster parameter is empty.
qsutils.assertQueryShapeConfiguration([]);

function setIndexes(coll, indexList) {
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndexes(indexList));
}

(function testDistinctQuerySettingsApplication() {
    // This query has the key that doesn't match any provided index which guarantees that there
    // would be no DISTINCT_SCAN plan and the query planner will fall back to the `find`. In case
    // multiplanner is involved it is expected that the query will end up in query plan cache.
    setIndexes(coll, [qstests.indexA, qstests.indexB, qstests.indexAB]);

    const querySettingsDistinctQuery = qsutils.makeDistinctQueryInstance({
        key: 'c',
        query: {a: 1, b: 1},
    });

    qstests.assertQuerySettingsIndexApplication(querySettingsDistinctQuery, ns);
    qstests.assertQuerySettingsNaturalApplication(querySettingsDistinctQuery, ns);
    qstests.assertQuerySettingsIgnoreCursorHints(querySettingsDistinctQuery, ns);
    // TODO SERVER-85242 Re-enable once the fallback mechanism is reimplemented.
    // qstests.assertQuerySettingsFallback(querySettingsDistinctQuery, ns);
    qstests.assertQuerySettingsCommandValidation(querySettingsDistinctQuery, ns);
})();

(function testDistinctWithDistinctScanQuerySettingsApplication() {
    // Here query planner is expected to produce DISTINCT_SCAN plans no matter which index is
    // hinted/set in query settings.
    // Only use those indexes that can be used in DISTINCT_SCAN.
    // query planner will never choose {b: 1} index for DISTINCT_SCAN plan for {key: 'a'} query, no
    // matter what hints/index_filters/query_settings are.
    setIndexes(coll, [qstests.indexA, qstests.indexAB]);

    const querySettingsDistinctQuery = qsutils.makeDistinctQueryInstance({key: 'a'});

    assertQuerySettingsDistinctIndexApplication(querySettingsDistinctQuery);
    assertQuerySettingsDistinctScanIgnoreCursorHints(querySettingsDistinctQuery);
    assertQuerySettingsDistinctFallback(querySettingsDistinctQuery);
})();
