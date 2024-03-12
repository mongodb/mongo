// Tests query settings are applied to aggregate queries regardless of the query engine (SBE or
// classic).
// @tags: [
//   # Explain on foreign sharded collections does not return used indexes.
//   assumes_unsharded_collection,
//   # $planCacheStats can not be run with specified read preferences/concerns.
//   assumes_read_preference_unchanged,
//   assumes_read_concern_unchanged,
//   # $planCacheStats can not be run in transactions.
//   does_not_support_transactions,
//   directly_against_shardsvrs_incompatible,
//   featureFlagQuerySettings,
//   simulate_atlas_proxy_incompatible,
//   cqf_incompatible,
//   # 'planCacheClear' command is not allowed with the security token.
//   not_allowed_with_signed_security_token,
//   # Explain for tracked unsharded collections return IXSCAN as inputStage
//   # TODO SERVER-87164 re-enable the tests in suites with random migrations
//   assumes_balancer_off,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsIndexHintsTests} from "jstests/libs/query_settings_index_hints_tests.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const mainNs = {
    db: db.getName(),
    coll: coll.getName()
};
const secondaryColl = assertDropAndRecreateCollection(db, "secondary");
const secondaryNs = {
    db: db.getName(),
    coll: secondaryColl.getName()
};
const qsutils = new QuerySettingsUtils(db, coll.getName());
const qstests = new QuerySettingsIndexHintsTests(qsutils);

// Insert data into the collection.
assert.commandWorked(coll.insertMany([
    {a: 1, b: 5},
    {a: 2, b: 4},
    {a: 3, b: 3},
    {a: 4, b: 2},
    {a: 5, b: 1},
]));

assert.commandWorked(secondaryColl.insertMany([
    {a: 1, b: 5},
    {a: 1, b: 5},
    {a: 3, b: 1},
]));

// Ensure that query settings cluster parameter is empty.
qsutils.assertQueryShapeConfiguration([]);

function setIndexes(coll, indexList) {
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndexes(indexList));
}
setIndexes(coll, [qstests.indexA, qstests.indexB, qstests.indexAB]);
setIndexes(secondaryColl, [qstests.indexA, qstests.indexB, qstests.indexAB]);

(function testAggregateQuerySettingsApplicationWithoutSecondaryCollections() {
    const aggregateCmd = qsutils.makeAggregateQueryInstance({
        pipeline: [{$match: {a: 1, b: 5}}],
        cursor: {},
    });
    qstests.assertQuerySettingsIndexApplication(aggregateCmd, mainNs);
    qstests.assertQuerySettingsIgnoreCursorHints(aggregateCmd, mainNs);
    // TODO SERVER-85242 Re-enable once the fallback mechanism is reimplemented.
    // qstests.assertQuerySettingsFallback(aggregateCmd, mainNs);
    qstests.assertQuerySettingsCommandValidation(aggregateCmd, mainNs);
})();

(function testAggregateQuerySettingsApplicationWithLookupEquiJoin() {
    const aggregateCmd = qsutils.makeAggregateQueryInstance({
    pipeline: [
      { $match: { a: 1, b: 5 } },
      {
        $lookup:
          { from: secondaryColl.getName(), localField: "a", foreignField: "a", as: "output" }
      }
    ],
    cursor: {},
  });

    // Ensure query settings index application for 'mainNs', 'secondaryNs' and both.
    qstests.assertQuerySettingsIndexApplication(aggregateCmd, mainNs);
    qstests.assertQuerySettingsLookupJoinIndexApplication(aggregateCmd, secondaryNs);
    qstests.assertQuerySettingsIndexAndLookupJoinApplications(aggregateCmd, mainNs, secondaryNs);

    // Ensure query settings ignore cursor hints when being set on main or secondary collection.
    qstests.assertQuerySettingsIgnoreCursorHints(aggregateCmd, mainNs);
    qstests.assertQuerySettingsIgnoreCursorHints(aggregateCmd, secondaryNs);

    // Ensure that providing query settings with an invalid index result in the same plan as no
    // query settings being set.
    // NOTE: The fallback is not tested when hinting secondary collections, as instead of fallback,
    // hash join or nlj will be used.
    // TODO: SERVER-86400 Add support for $natural hints on secondary collections.
    // TODO SERVER-85242 Re-enable once the fallback mechanism is reimplemented.
    // qstests.assertQuerySettingsFallback(aggregateCmd, mainNs);

    qstests.assertQuerySettingsCommandValidation(aggregateCmd, mainNs);
    qstests.assertQuerySettingsCommandValidation(aggregateCmd, secondaryNs);
})();

(function testAggregateQuerySettingsApplicationWithLookupPipeline() {
    const aggregateCmd = qsutils.makeAggregateQueryInstance({
    aggregate: coll.getName(),
    pipeline: [
      { $match: { a: 1, b: 5 } },
      {
        $lookup:
          { from: secondaryColl.getName(), pipeline: [{ $match: { a: 1, b: 5 } }], as: "output" }
      }
    ],
    cursor: {},
  });

    // Ensure query settings index application for 'mainNs', 'secondaryNs' and both.
    qstests.assertQuerySettingsIndexApplication(aggregateCmd, mainNs);
    qstests.assertQuerySettingsLookupPipelineIndexApplication(aggregateCmd, secondaryNs);
    qstests.assertQuerySettingsIndexAndLookupPipelineApplications(
        aggregateCmd, mainNs, secondaryNs);

    // Ensure query settings ignore cursor hints when being set on main collection.
    qstests.assertQuerySettingsIgnoreCursorHints(aggregateCmd, mainNs);

    // Ensure both cursor hints and query settings are applied, since they are specified on
    // different pipelines.
    qstests.assertQuerySettingsWithCursorHints(aggregateCmd, mainNs, secondaryNs);

    // TODO SERVER-85242 Re-enable once the fallback mechanism is reimplemented.
    // qstests.assertQuerySettingsFallback(aggregateCmd, mainNs);
    // qstests.assertQuerySettingsFallback(aggregateCmd, secondaryNs);

    qstests.assertQuerySettingsCommandValidation(aggregateCmd, mainNs);
    qstests.assertQuerySettingsCommandValidation(aggregateCmd, secondaryNs);
})();
