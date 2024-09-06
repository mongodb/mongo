// Tests query settings are applied to aggregate queries regardless of the query engine (SBE or
// classic).
// @tags: [
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
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {QuerySettingsIndexHintsTests} from "jstests/libs/query_settings_index_hints_tests.js";
import {QuerySettingsUtils} from "jstests/libs/query_settings_utils.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const viewName = "identityView";
assertDropCollection(db, viewName);
assert.commandWorked(db.createView(viewName, coll.getName(), []));
const mainNs = {
    db: db.getName(),
    coll: coll.getName()
};
const secondaryColl = assertDropAndRecreateCollection(db, "secondary");
const secondaryViewName = "secondaryIdentityView";
assertDropCollection(db, secondaryViewName);
assert.commandWorked(db.createView(secondaryViewName, secondaryColl.getName(), []));
const secondaryNs = {
    db: db.getName(),
    coll: secondaryColl.getName()
};

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

new QuerySettingsUtils(db, coll.getName()).removeAllQuerySettings();

function setIndexes(coll, indexList) {
    assert.commandWorked(coll.dropIndexes());
    assert.commandWorked(coll.createIndexes(indexList));
}

function testAggregateQuerySettingsApplicationWithoutSecondaryCollections(collOrViewName) {
    const qsutils = new QuerySettingsUtils(db, collOrViewName);
    const qstests = new QuerySettingsIndexHintsTests(qsutils);

    setIndexes(coll, [qstests.indexA, qstests.indexB, qstests.indexAB]);

    // Ensure that query settings cluster parameter is empty.
    qsutils.assertQueryShapeConfiguration([]);

    const aggregateCmd = qsutils.makeAggregateQueryInstance({pipeline: [{$match: {a: 1, b: 5}}]});
    qstests.assertQuerySettingsIndexApplication(aggregateCmd, mainNs);
    qstests.assertQuerySettingsIgnoreCursorHints(aggregateCmd, mainNs);
    qstests.assertQuerySettingsFallback(aggregateCmd, mainNs);
    qstests.assertQuerySettingsCommandValidation(aggregateCmd, mainNs);
}

function testAggregateQuerySettingsApplicationWithLookupEquiJoin(
    collOrViewName, secondaryCollOrViewName, isSecondaryCollAView) {
    const qsutils = new QuerySettingsUtils(db, collOrViewName);
    const qstests = new QuerySettingsIndexHintsTests(qsutils);

    // Set indexes on both collections.
    setIndexes(coll, [qstests.indexA, qstests.indexB, qstests.indexAB]);
    setIndexes(secondaryColl, [qstests.indexA, qstests.indexB, qstests.indexAB]);

    // Ensure that query settings cluster parameter is empty.
    qsutils.assertQueryShapeConfiguration([]);

    const aggregateCmd = qsutils.makeAggregateQueryInstance({
    pipeline: [
      { $match: { a: 1, b: 5 } },
      {
        $lookup:
          { from: secondaryCollOrViewName, localField: "a", foreignField: "a", as: "output" }
      },
      // Ensure that the pipeline is only partially pushed down to SBE to verify its 
      // integrity after the fallback mechanism is engaged.
      { $_internalInhibitOptimization: {} },
      { $limit: 1 },
    ]});

    // Ensure query settings index application for 'mainNs', 'secondaryNs' and both.
    qstests.assertQuerySettingsIndexApplication(aggregateCmd, mainNs);
    qstests.assertQuerySettingsLookupJoinIndexApplication(
        aggregateCmd, secondaryNs, isSecondaryCollAView);
    qstests.assertQuerySettingsIndexAndLookupJoinApplications(
        aggregateCmd, mainNs, secondaryNs, isSecondaryCollAView);

    if (!isSecondaryCollAView) {
        qstests.testAggregateQuerySettingsNaturalHintEquiJoinStrategy(
            aggregateCmd, mainNs, secondaryNs);
        qstests.testAggregateQuerySettingsNaturalHintDirectionWhenSecondaryHinted(
            aggregateCmd, mainNs, secondaryNs);
    }

    // Ensure query settings ignore cursor hints when being set on main collection.
    qstests.assertQuerySettingsIgnoreCursorHints(aggregateCmd, mainNs);
    if (checkSbeRestrictedOrFullyEnabled(db) && !isSecondaryCollAView) {
        // The aggregation stage will get pushed down to SBE, and index hints will get applied to
        // secondary collections. This prevents cursor hints from also being applied.
        qstests.assertQuerySettingsIgnoreCursorHints(aggregateCmd, secondaryNs);
    } else {
        // No SBE push down happens. The $lookup will get executed as a separate pipeline, so we
        // expect cursor hints to be applied on the main collection, while query settings will get
        // applied on the secondary collection.
        qstests.assertQuerySettingsWithCursorHints(aggregateCmd, mainNs, secondaryNs);
    }

    // Ensure that providing query settings with an invalid index result in the same plan as no
    // query settings being set.
    qstests.assertQuerySettingsFallback(aggregateCmd, mainNs);
    qstests.assertQuerySettingsFallback(aggregateCmd, secondaryNs);

    qstests.assertQuerySettingsCommandValidation(aggregateCmd, mainNs);
    qstests.assertQuerySettingsCommandValidation(aggregateCmd, secondaryNs);
}

function testAggregateQuerySettingsApplicationWithLookupPipeline(collOrViewName,
                                                                 secondaryCollOrViewName) {
    const qsutils = new QuerySettingsUtils(db, collOrViewName);
    const qstests = new QuerySettingsIndexHintsTests(qsutils);

    // Set indexes on both collections.
    setIndexes(coll, [qstests.indexA, qstests.indexB, qstests.indexAB]);
    setIndexes(secondaryColl, [qstests.indexA, qstests.indexB, qstests.indexAB]);

    // Ensure that query settings cluster parameter is empty.
    qsutils.assertQueryShapeConfiguration([]);

    const aggregateCmd = qsutils.makeAggregateQueryInstance({
    pipeline: [
      { $match: { a: 1, b: 5 } },
      {
        $lookup:
          { from: secondaryCollOrViewName, pipeline: [{ $match: { a: 1, b: 5 } }], as: "output" }
      },
    ]});

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

    qstests.assertQuerySettingsFallback(aggregateCmd, mainNs);
    qstests.assertQuerySettingsFallback(aggregateCmd, secondaryNs);

    qstests.assertQuerySettingsCommandValidation(aggregateCmd, mainNs);
    qstests.assertQuerySettingsCommandValidation(aggregateCmd, secondaryNs);
}

function testAggregateQuerySettingsApplicationWithGraphLookup(collOrViewName,
                                                              secondaryCollOrViewName) {
    const qsutils = new QuerySettingsUtils(db, collOrViewName);
    const qstests = new QuerySettingsIndexHintsTests(qsutils);

    // Set indexes on both collections.
    setIndexes(coll, [qstests.indexA, qstests.indexB, qstests.indexAB]);
    setIndexes(secondaryColl, [qstests.indexA, qstests.indexB, qstests.indexAB]);

    // Ensure that query settings cluster parameter is empty.
    qsutils.assertQueryShapeConfiguration([]);

    const filter = {a: {$ne: "Bond"}, b: {$ne: "James"}};
    const pipeline = [{
        $match: filter
        }, {
        $graphLookup: {
        from: secondaryCollOrViewName,
        startWith: "$a",
        connectFromField: "b",
        connectToField: "a",
        as: "children",
        maxDepth: 4,
        depthField: "depth",
        restrictSearchWithMatch: filter
        }
    }];
    const aggregateCmd = qsutils.makeAggregateQueryInstance({pipeline});

    // Ensure query settings index application for 'mainNs'.
    // TODO SERVER-88561: Ensure query settings index application for 'secondaryNs' after
    // 'indexesUsed' is added to the 'explain' command output for the $graphLookup operation.
    qstests.assertQuerySettingsIndexApplication(aggregateCmd, mainNs);
    qstests.assertGraphLookupQuerySettingsInCache(aggregateCmd, secondaryNs);
}

function testAggregateQuerySettingsApplicationWithUnionWithPipeline(collOrViewName,
                                                                    secondaryCollOrViewName) {
    const qsutils = new QuerySettingsUtils(db, collOrViewName);
    const qstests = new QuerySettingsIndexHintsTests(qsutils);

    // Set indexes on both collections.
    setIndexes(coll, [qstests.indexA, qstests.indexB, qstests.indexAB]);
    setIndexes(secondaryColl, [qstests.indexA, qstests.indexB, qstests.indexAB]);

    // Ensure that query settings cluster parameter is empty.
    qsutils.assertQueryShapeConfiguration([]);

    const aggregateCmd = qsutils.makeAggregateQueryInstance({
        pipeline: [
            {$match: {a: 1, b: 5}},
            {$unionWith: {coll: secondaryCollOrViewName, pipeline: [{$match: {b: 5, a: 1}}]}}
        ]
    });

    // Ensure query settings index application for 'mainNs', 'secondaryNs' and both.
    qstests.assertQuerySettingsIndexApplication(aggregateCmd, mainNs);
    qstests.assertQuerySettingsIndexApplication(aggregateCmd, secondaryNs);
    qstests.assertQuerySettingsIndexApplications(aggregateCmd, mainNs, secondaryNs);

    // Ensure query settings ignore cursor hints when being set on main collection.
    qstests.assertQuerySettingsIgnoreCursorHints(aggregateCmd, mainNs);

    // Ensure both cursor hints and query settings are applied, since they are specified on
    // different pipelines.
    qstests.assertQuerySettingsWithCursorHints(aggregateCmd, mainNs, secondaryNs);

    qstests.assertQuerySettingsFallback(aggregateCmd, mainNs);
    qstests.assertQuerySettingsFallback(aggregateCmd, secondaryNs);

    qstests.assertQuerySettingsCommandValidation(aggregateCmd, mainNs);
    qstests.assertQuerySettingsCommandValidation(aggregateCmd, secondaryNs);
}

// Execute each provided test case for each combination of collection/view for main/secondary
// collections.
function instantiateTestCases(...testCases) {
    for (const testCase of testCases) {
        testCase(coll.getName(), secondaryColl.getName(), false);
        testCase(viewName, secondaryColl.getName(), false);
        testCase(coll.getName(), secondaryViewName, true);
        testCase(viewName, secondaryViewName, true);
    }
}

// Execute each provided test case for collection/view for main collection, and collection only for
// secondary collection.
function instantiateTestCasesNoSecondaryView(...testCases) {
    for (const testCase of testCases) {
        testCase(coll.getName(), secondaryColl.getName(), false);
        testCase(viewName, secondaryColl.getName(), false);
    }
}

if (FixtureHelpers.isSharded(coll) || FixtureHelpers.isSharded(secondaryColl)) {
    // TODO: SERVER-88883 Report 'indexesUsed' for $lookup over sharded collections.
    instantiateTestCases(
        testAggregateQuerySettingsApplicationWithGraphLookup,
        testAggregateQuerySettingsApplicationWithUnionWithPipeline,
    );

    instantiateTestCasesNoSecondaryView(
        testAggregateQuerySettingsApplicationWithoutSecondaryCollections);
} else {
    instantiateTestCases(
        testAggregateQuerySettingsApplicationWithLookupEquiJoin,
        testAggregateQuerySettingsApplicationWithLookupPipeline,
        testAggregateQuerySettingsApplicationWithGraphLookup,
        testAggregateQuerySettingsApplicationWithUnionWithPipeline,
    );

    instantiateTestCasesNoSecondaryView(
        testAggregateQuerySettingsApplicationWithoutSecondaryCollections);
}
