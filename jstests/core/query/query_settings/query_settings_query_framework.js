// Tests query settings impact on the queries when 'queryFramework' is set.
// @tags: [
//   # TODO SERVER-98659 Investigate why this test is failing on
//   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
//   does_not_support_stepdowns,
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   requires_fcv_80,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

// Create the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());
qsutils.removeAllQuerySettings();

// Create the index, such that we can ensure that index hints can be combined with query settings,
// when query settings specify only query engine version.
const indexKeyPattern = {
    a: 1,
    b: 1
};
assert.commandWorked(coll.createIndexes([{a: 1}, indexKeyPattern, {a: 1, b: 1, c: 1}]));

const sbeEligibleQuery = {
    find: coll.getName(),
    $db: db.getName(),
    filter: {a: {$lt: 2}},
    hint: indexKeyPattern,
};

const nonSbeEligibleQuery = {
    find: coll.getName(),
    $db: db.getName(),
    sort: {"a.0": 1},
    hint: indexKeyPattern,
};

const sbeRestrictedQuery = qsutils.makeAggregateQueryInstance({
    pipeline: [{$group: {_id: "$groupID", count: {$sum: 1}}}],
    hint: indexKeyPattern,
});

const nonSbeRestrictedQuery = qsutils.makeAggregateQueryInstance({
    pipeline: [{$match: {a: 1}}, {$project: {a: 1}}],
    hint: indexKeyPattern,
});

qsutils.assertQueryFramework({
    query: sbeEligibleQuery,
    settings: {queryFramework: "classic"},
    expectedEngine: "classic",
});

qsutils.assertQueryFramework({
    query: sbeEligibleQuery,
    settings: {queryFramework: "sbe"},
    expectedEngine: "sbe",
});

qsutils.assertQueryFramework({
    query: nonSbeEligibleQuery,
    settings: {queryFramework: "classic"},
    expectedEngine: "classic",
});

qsutils.assertQueryFramework({
    query: nonSbeEligibleQuery,
    settings: {queryFramework: "sbe"},
    expectedEngine: "classic",
});

qsutils.assertQueryFramework({
    query: sbeRestrictedQuery,
    settings: {queryFramework: "classic"},
    expectedEngine: "classic",
});

qsutils.assertQueryFramework({
    query: sbeRestrictedQuery,
    settings: {queryFramework: "sbe"},
    expectedEngine: "sbe",
});

qsutils.assertQueryFramework({
    query: nonSbeRestrictedQuery,
    settings: {queryFramework: "classic"},
    expectedEngine: "classic",
});

qsutils.assertQueryFramework({
    query: nonSbeRestrictedQuery,
    settings: {queryFramework: "sbe"},
    expectedEngine: "sbe",
});
