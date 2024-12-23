// Tests that internal/administrative queries cannot be rejected via the use of `reject` in query
// settings.
// @tags: [
//   # TODO SERVER-98659 Investigate why this test is failing on
//   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
//   does_not_support_stepdowns,
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   assumes_read_preference_unchanged,
//   assumes_read_concern_unchanged,
//   does_not_support_transactions,
//   requires_fcv_80,
//   simulate_mongoq_incompatible,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

// Creating the collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());
const adminDB = db.getSiblingDB("admin");
const configDB = db.getSiblingDB("config");

// Stages which, if present in a pipeline, prevent the application of query settings.
const stages = [
    //[Stage name, db, collection/1]
    ["$querySettings", db, 1],
    ["$planCacheStats", db, coll.getName()],
    ["$collStats", db, coll.getName()],
    ["$indexStats", db, coll.getName()],
    // The following stages cannot normally have query settings set, as they require an "internal"
    // db or collection. However, this test still verifies that reject=true is ignored if present.
    ["$listSessions", configDB, "system.sessions"],
    ["$listSampledQueries", adminDB, 1],
    ["$queryStats", adminDB, 1],
    ["$currentOp", adminDB, 1],
    ["$listCatalog", adminDB, 1],
    ["$listLocalSessions", adminDB, 1],
];

// The following stages cannot be tested here:
// Requires mongot
//  ["$listSearchIndexes", db, coll.getName()],

// Requires server parameter aggregateOperationResourceConsumptionMetrics
//  ["$operationMetrics", adminDB, 1],

// Agg queries containing the above stages.
const queries =
    stages.map(([stage, db, collection]) =>
                   new QuerySettingsUtils(db, collection)
                       .makeAggregateQueryInstance({pipeline: [{[stage]: {}}], cursor: {}}));

// Reset query settings.
qsutils.removeAllQuerySettings();

for (const query of queries) {
    const dbName = query["$db"];
    // Verify the request works before reject is set
    assert.commandWorked(db.getSiblingDB(dbName).runCommand(qsutils.withoutDollarDB(query)));

    // Should not be able to set reject=true using a representative query.
    assert.commandFailedWithCode(
        db.adminCommand({setQuerySettings: query, settings: {reject: true}}),
        [8584900 /* internal */, 8705200 /* forbidden by stage */],
        "It should not be possible to set reject=true for query: " + JSON.stringify(query));

    // To be able to conveniently test what happens if reject _is_ set (e.g., in production, by
    // query shape hash), temporarily bypass restrictions on setQuerySettings.
    const allowAllSetQuerySettingsFailPoint =
        configureFailPoint(db.getMongo(), "allowAllSetQuerySettings");

    qsutils.withQuerySettings(query, {reject: true}, () => {
        // Verify the query still works, despite reject=true being set.
        assert.commandWorked(db.getSiblingDB(dbName).runCommand(qsutils.withoutDollarDB(query)));
    });

    // Clear the failpoint to return to normal validation rules.
    allowAllSetQuerySettingsFailPoint.off();
}
