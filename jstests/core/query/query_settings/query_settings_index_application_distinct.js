// Tests query settings are applied to distinct queries regardless of the query engine (SBE or
// classic).
// @tags: [
//   # TODO SERVER-98659 Investigate why this test is failing on
//   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
//   does_not_support_stepdowns,
//   # Balancer may impact the explain output (e.g. data was previously present on both shards and
//   # now only on one).
//   assumes_balancer_off,
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   # 'planCacheClear' command is not allowed with the security token.
//   not_allowed_with_signed_security_token,
//   # Test includes SBE plan cache assertions if the SBE plan cache is used.
//   examines_sbe_cache,
// ]
//

import {assertDropAndRecreateCollection, assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsIndexHintsTests} from "jstests/libs/query/query_settings_index_hints_tests.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const isTimeseriesTestSuite = TestData.isTimeseriesTestSuite || false;

// Create the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const viewName = "identityView";
assertDropCollection(db, viewName);
assert.commandWorked(db.createView(viewName, coll.getName(), []));
const ns = {
    db: db.getName(),
    coll: coll.getName(),
};

// Ensure query settings are applied as expected in a straightforward scenario for distinct.
function assertQuerySettingsDistinctIndexApplication(qsutils, qstests, querySettingsQuery) {
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
function assertQuerySettingsDistinctScanIgnoreCursorHints(qsutils, qstests, querySettingsQuery) {
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
function assertQuerySettingsDistinctFallback(qsutils, qstests, querySettingsQuery) {
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
assert.commandWorked(
    coll.insertMany([
        {a: 1, b: 5},
        {a: 2, b: 4},
        {a: 3, b: 3},
        {a: 4, b: 2},
        {a: 5, b: 1},
    ]),
);

function setIndexes(coll, indexList) {
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndexes(indexList));
}

function testDistinctQuerySettingsApplication(collOrViewName) {
    const qsutils = new QuerySettingsUtils(db, collOrViewName);
    const qstests = new QuerySettingsIndexHintsTests(qsutils);

    // This query has the key that doesn't match any provided index which guarantees that there
    // would be no DISTINCT_SCAN plan and the query planner will fall back to the `find`. In case
    // multiplanner is involved it is expected that the query will end up in query plan cache.
    setIndexes(coll, qstests.allIndexes);

    const querySettingsDistinctQuery = qsutils.makeDistinctQueryInstance({
        key: "c",
        query: {a: 1, b: 1},
    });

    qstests.assertQuerySettingsIndexApplication(querySettingsDistinctQuery, ns);
    qstests.assertQuerySettingsNaturalApplication(querySettingsDistinctQuery, ns);
    qstests.assertQuerySettingsIgnoreCursorHints(querySettingsDistinctQuery, ns);
    qstests.assertQuerySettingsFallback(querySettingsDistinctQuery, ns);
    qstests.assertQuerySettingsFallbackNoQueryExecutionPlans(querySettingsDistinctQuery, ns);
    qstests.assertQuerySettingsCommandValidation(querySettingsDistinctQuery, ns);
}

function testDistinctWithDistinctScanQuerySettingsApplication(collOrViewName) {
    const qsutils = new QuerySettingsUtils(db, collOrViewName);
    const qstests = new QuerySettingsIndexHintsTests(qsutils);

    // Here query planner is expected to produce DISTINCT_SCAN plans no matter which index is
    // hinted/set in query settings.
    // Only use those indexes that can be used in DISTINCT_SCAN.
    // query planner will never choose {b: 1} index for DISTINCT_SCAN plan for {key: 'a'} query, no
    // matter what hints/index_filters/query_settings are.
    setIndexes(coll, [qstests.indexA, qstests.indexAB]);

    const querySettingsDistinctQuery = qsutils.makeDistinctQueryInstance({key: "a"});

    assertQuerySettingsDistinctIndexApplication(qsutils, qstests, querySettingsDistinctQuery);
    assertQuerySettingsDistinctScanIgnoreCursorHints(qsutils, qstests, querySettingsDistinctQuery);
    assertQuerySettingsDistinctFallback(qsutils, qstests, querySettingsDistinctQuery);
}

testDistinctQuerySettingsApplication(coll.getName());
testDistinctQuerySettingsApplication(viewName);
// TODO: SERVER-87741: Make distinct command on views use DISTINCT_SCAN.
if (!isTimeseriesTestSuite) {
    testDistinctWithDistinctScanQuerySettingsApplication(coll.getName());
}

// The view can persist in the system.views collection, which may cause issues if subsequent tests
// check this collection.
assert(db[viewName].drop(), "couldn't drop view");

// TODO: SERVER-87741: Make distinct command on views use DISTINCT_SCAN.
// testDistinctWithDistinctScanQuerySettingsApplication(viewName);
