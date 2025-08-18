/**
 * Verifies that features added in SPM-4257: Close feature gap between MQL and JS engine behave
 * correctly in FCV upgrade/downgrade scenarios.
 *
 * @tags: [
 *   featureFlagMqlJsEngineGap
 * ]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    testPerformUpgradeDowngradeReplSet
} from "jstests/multiVersion/libs/mixed_version_fixture_test.js";
import {
    testPerformUpgradeDowngradeSharded
} from "jstests/multiVersion/libs/mixed_version_sharded_fixture_test.js";

const collectionName = "coll";

// These expression just exhibit new behavior, but no new syntax, and can therefore still be parsed
// by old versions.
const expressionsWithNewFeaturesThatCanBeParsedByOldFCV = [
    {
        viewName: "replaceOneView",
        expression: {
            $replaceOne: {
                input: "$replaceParams.input",
                find: "$replaceParams.find",
                replacement: "$replaceParams.replacement"
            }
        },
        failureErrorCode: [51745, 10503901]
    },
    {
        viewName: "replaceAllView",
        expression: {
            $replaceAll: {
                input: "$replaceParams.input",
                find: "$replaceParams.find",
                replacement: "$replaceParams.replacement"
            }
        },
        failureErrorCode: [51745, 10503901]
    },
    {
        viewName: "splitView",
        expression: {$split: ["$splitParams.input", "$splitParams.delimiter"]},
        failureErrorCode: [40086, 10503900]
    },
];

// These expressions are either completely new, or have new syntactic parameters, that
// didn't exist before. These expression can not be parsed by older versions.
const expressionsWithNewFeaturesThatCannotBeParsedByOldFCV = [
    {
        viewName: "createObjectIdView",
        expression: {$createObjectId: {}},
        failureErrorCode: [31325, ErrorCodes.QueryFeatureNotAllowed]
    },
    {
        viewName: "convertToNumberView",
        expression: {
            $convert: {
                input: "$convertToNumberParams.input",
                to: "$convertToNumberParams.to",
                base: "$convertToNumberParams.base"
            }
        },
        failureErrorCode: [ErrorCodes.FailedToParse]
    },
    {
        viewName: "convertToStringView",
        expression: {
            $convert: {
                input: "$convertToStringParams.input",
                to: "$convertToStringParams.to",
                base: "$convertToStringParams.base"
            }
        },
        failureErrorCode: [ErrorCodes.FailedToParse]
    },
    {
        viewName: "subtypeView",
        expression: {$subtype: "$binDataInput"},
        failureErrorCode: [31325, ErrorCodes.QueryFeatureNotAllowed]
    },
];

const getDB = (primaryConnection) => primaryConnection.getDB(jsTestName());

function setupCollection(primaryConnection, shardingTest = null) {
    const coll = assertDropAndRecreateCollection(getDB(primaryConnection), collectionName);

    if (shardingTest) {
        // Shard on _id to ensure documents are distributed across both shards.
        shardingTest.shardColl(coll, {_id: 1}, {_id: 1});
    }

    assert.commandWorked(coll.insertMany([
        {
            _id: 0,
            replaceParams: {input: "123-456-7890", find: /\d{3}/, replacement: "xxx"},
            splitParams: {input: "abacd", delimiter: /(a)(b)/},
            convertToNumberParams: {input: "1010", to: "int", base: 2},
            convertToStringParams: {input: NumberInt("10"), to: "string", base: 2},
            binDataInput: BinData(0, "CQDoAwAAAAAAAAA="),
        },
        {
            _id: 1,
            replaceParams: {input: "line1\nline2", find: /^line/m, replacement: "start: "},
            splitParams: {input: "abacd", delimiter: /(a)(b)/},
            convertToNumberParams: {input: "12", to: "long", base: 8},
            convertToStringParams: {input: NumberLong("10"), to: "string", base: 8},
            binDataInput: UUID("81fd5473-1747-4c9d-8743-f10642b3bb99"),
        },
        {
            _id: 2,
            replaceParams: {input: "helloworld", find: /([aeiou]+)/, replacement: "X"},
            splitParams: {input: "abacd", delimiter: /(a)(b)/},
            convertToNumberParams: {input: "10", to: "double", base: 10},
            convertToStringParams: {input: 10, to: "string", base: 10},
            binDataInput: BinData(4, "CQDoAwAAAAAAAAA="),
        },
        {
            _id: 3,
            replaceParams: {input: "123.456.7890", find: /([0-9]+)(\.)/, replacement: "x"},
            splitParams: {input: "abacd", delimiter: /(a)(b)/},
            convertToNumberParams: {input: "A", to: "decimal", base: 16},
            convertToStringParams: {input: NumberDecimal("10"), to: "string", base: 16},
            binDataInput: BinData(128, "CQDoAwAAAAAAAAA="),
        },
    ]));

    if (shardingTest) {
        // Verify that documents are distributed across both shards
        const shardCounts = shardingTest.shardCounts(jsTestName(), collectionName);
        assert.gt(shardCounts[0], 0);
        assert.gt(shardCounts[1], 0);
    }
}

function assertCreateAndEvaluateViewsWithNewFeaturesPass(primaryConnection) {
    const db = getDB(primaryConnection);

    // All expression should pass, so we can create a view for each of them and evaluate that view.
    for (const expr of [...expressionsWithNewFeaturesThatCanBeParsedByOldFCV,
                        ...expressionsWithNewFeaturesThatCannotBeParsedByOldFCV]) {
        db[expr.viewName].drop();
        assert.commandWorked(
            db.createView(expr.viewName, collectionName, [{$project: {output: expr.expression}}]));
        assert.commandWorked(db.runCommand({find: expr.viewName, filter: {}}));
        db[expr.viewName].drop();

        // Evaluating the expression without the view also passes.
        assert.commandWorked(db.runCommand({
            aggregate: collectionName,
            cursor: {},
            pipeline: [{$project: {output: expr.expression}}]
        }));
    }
}

function assertCreateOrEvaluateViewsWithNewFeaturesFail(primaryConnection) {
    const db = getDB(primaryConnection);

    // Making views using expression that can be parsed, is possible, but evaluating them won't
    // work.
    for (const expr of expressionsWithNewFeaturesThatCanBeParsedByOldFCV) {
        db[expr.viewName].drop();
        assert.commandWorked(
            db.createView(expr.viewName, collectionName, [{$project: {output: expr.expression}}]));
        assert.commandFailedWithCode(db.runCommand({find: expr.viewName, filter: {}}),
                                     expr.failureErrorCode);
        db[expr.viewName].drop();
    }

    // It is not possible to make a view that uses expressions with new syntax.
    for (const expr of expressionsWithNewFeaturesThatCannotBeParsedByOldFCV) {
        db[expr.viewName].drop();
        assert.commandFailedWithCode(
            db.createView(expr.viewName, collectionName, [{$project: {output: expr.expression}}]),
            expr.failureErrorCode);
    }

    // Running new expressions in an aggregation pipeline will result in an error.
    for (const expr of [...expressionsWithNewFeaturesThatCanBeParsedByOldFCV,
                        ...expressionsWithNewFeaturesThatCannotBeParsedByOldFCV]) {
        assert.commandFailedWithCode(db.runCommand({
            aggregate: collectionName,
            cursor: {},
            pipeline: [{$project: {output: expr.expression}}]
        }),
                                     expr.failureErrorCode);
    }
}

testPerformUpgradeDowngradeReplSet({
    setupFn: setupCollection,
    whenFullyDowngraded: assertCreateOrEvaluateViewsWithNewFeaturesFail,
    whenSecondariesAreLatestBinary: assertCreateOrEvaluateViewsWithNewFeaturesFail,
    whenBinariesAreLatestAndFCVIsLastLTS: assertCreateOrEvaluateViewsWithNewFeaturesFail,
    whenFullyUpgraded: assertCreateAndEvaluateViewsWithNewFeaturesPass,
});

testPerformUpgradeDowngradeSharded({
    setupFn: setupCollection,
    whenFullyDowngraded: assertCreateOrEvaluateViewsWithNewFeaturesFail,
    whenOnlyConfigIsLatestBinary: assertCreateOrEvaluateViewsWithNewFeaturesFail,
    whenSecondariesAndConfigAreLatestBinary: assertCreateOrEvaluateViewsWithNewFeaturesFail,
    whenMongosBinaryIsLastLTS: assertCreateOrEvaluateViewsWithNewFeaturesFail,
    whenBinariesAreLatestAndFCVIsLastLTS: assertCreateOrEvaluateViewsWithNewFeaturesFail,
    whenFullyUpgraded: assertCreateAndEvaluateViewsWithNewFeaturesPass,
});
