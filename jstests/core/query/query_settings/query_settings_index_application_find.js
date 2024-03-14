// Tests query settings are applied to find queries regardless of the query engine (SBE or classic).
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

(function testFindQuerySettingsApplication() {
    setIndexes(coll, [qstests.indexA, qstests.indexB, qstests.indexAB]);
    const querySettingsFindQuery = qsutils.makeFindQueryInstance({
        filter: {a: 1, b: 1},
        // The skip-clause is a part of the query shape, however, it is not propagated to the shards
        // in a sharded cluster. Nevertheless, the shards should use the query settings matching the
        // original query shape.
        skip: 3,
    });

    qstests.assertQuerySettingsIndexApplication(querySettingsFindQuery, ns);
    qstests.assertQuerySettingsNaturalApplication(querySettingsFindQuery, ns);
    qstests.assertQuerySettingsIgnoreCursorHints(querySettingsFindQuery, ns);
    // TODO SERVER-85242 Re-enable once the fallback mechanism is reimplemented.
    // qstests.assertQuerySettingsFallback(querySettingsFindQuery, ns);
    qstests.assertQuerySettingsCommandValidation(querySettingsFindQuery, ns);
})();
