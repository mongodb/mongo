// Tests query settings can be set and applied on strict API commands.
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
//   # The test sets API parameter values.
//   uses_api_parameters,
//   requires_fcv_80
// ]
//

import {
    assertDropAndRecreateCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsIndexHintsTests} from "jstests/libs/query/query_settings_index_hints_tests.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const ns = {
    db: db.getName(),
    coll: coll.getName()
};
const qsutils = new QuerySettingsUtils(db, coll.getName());
const qstests = new QuerySettingsIndexHintsTests(qsutils);

const apiStrictOpts = {
    apiVersion: "1",
    apiStrict: true
};

// Create indexes on field a and b.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndexes(qstests.allIndexes));

// Insert data into the collection.
assert.commandWorked(coll.insertMany([
    {a: 1, b: 5},
    {a: 2, b: 4},
    {a: 3, b: 3},
    {a: 4, b: 2},
    {a: 5, b: 1},
]));

// Ensure query settings can be set and applied on a strict API find command.
{
    const findCmd = {find: coll.getName(), filter: {a: 1, b: 1}, ...apiStrictOpts};
    qstests.assertQuerySettingsIndexApplication(qsutils.makeQueryInstance(findCmd), ns);
}

// Ensure query settings can be set and applied on a strict API aggregate command.
{
    const aggCmd = {
        aggregate: coll.getName(),
        pipeline: [{$match: {a: 1, b: 1}}],
        ...apiStrictOpts
    };
    qstests.assertQuerySettingsIndexApplication(qsutils.makeQueryInstance(aggCmd), ns);
}
