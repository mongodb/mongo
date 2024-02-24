// Tests query settings impact on the queries when 'queryFramework' is set.
// @tags: [
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   simulate_atlas_proxy_incompatible,
//   cqf_incompatible,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

// Create the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());

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
