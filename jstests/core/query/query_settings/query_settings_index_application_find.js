// Tests query settings are applied to find queries regardless of the query engine (SBE or classic).
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
//   requires_fcv_80,
// ]
//

import {
    assertDropAndRecreateCollection,
    assertDropCollection
} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsIndexHintsTests} from "jstests/libs/query/query_settings_index_hints_tests.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

// Create the collection, because some sharding passthrough suites are failing when explain
// command is issued on the nonexistent database and collection.
const coll = assertDropAndRecreateCollection(db, jsTestName());
const viewName = "identityView";
assertDropCollection(db, viewName);
assert.commandWorked(db.createView(viewName, coll.getName(), []));
const ns = {
    db: db.getName(),
    coll: coll.getName()
};

// Insert data into the collection.
assert.commandWorked(coll.insertMany([
    {a: 1, b: 5},
    {a: 2, b: 4},
    {a: 3, b: 3},
    {a: 4, b: 2},
    {a: 5, b: 1},
]));

function setIndexes(coll, indexList) {
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndexes(indexList));
}

function testFindQuerySettingsApplication(collOrViewName) {
    const qsutils = new QuerySettingsUtils(db, collOrViewName);
    const qstests = new QuerySettingsIndexHintsTests(qsutils);

    setIndexes(coll, [qstests.indexA, qstests.indexB, qstests.indexAB]);

    // Ensure that there are no query settings set.
    qsutils.removeAllQuerySettings();

    const querySettingsFindQuery = qsutils.makeFindQueryInstance({
        filter: {a: 1, b: 1},
        // The skip-clause is a part of the query shape, however, it is not propagated to the shards
        // in a sharded cluster. Nevertheless, the shards should use the query settings matching the
        // original query shape.
        skip: 3,
        let : {
            c: 1,
            d: 2,
        }
    });

    qstests.assertQuerySettingsIndexApplication(querySettingsFindQuery, ns);
    qstests.assertQuerySettingsNaturalApplication(querySettingsFindQuery, ns);
    qstests.assertQuerySettingsIgnoreCursorHints(querySettingsFindQuery, ns);
    qstests.assertQuerySettingsFallback(querySettingsFindQuery, ns);
    qstests.assertQuerySettingsCommandValidation(querySettingsFindQuery, ns);
}

testFindQuerySettingsApplication(coll.getName());
testFindQuerySettingsApplication(viewName);
