/**
 * Test that $queryStats properly tokenizes update (where the update modification is specified
 * as replacement document or pipeline) commands on mongod.
 *
 * @tags: [requires_fcv_83]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    getQueryStatsUpdateCmd,
    withQueryStatsEnabled,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();

const kHashedFieldName = "lU7Z0mLRPRUL+RfAD5jhYPRRpXBsZBxS/20EzDwfOG4="; // Hash of field "v"
const kHashedIdField = "+0wgDp/AI7f+XT+DJEqixDyZBq9zRe7RGN0wCS9bd94="; // Hash of _id
const kHashedFieldForA = "GDiF6ZEXkeo4kbKyKEAAViZ+2RHIVxBQV9S6b6Lu7gU="; // Hash of field "a"

//
// Replacement update tests.
//

function testReplacementUpdateTokenization(testDB, collName) {
    const cmdObj = {
        update: collName,
        updates: [{q: {v: 1}, u: {v: 100}}],
        comment: "replacement update!",
    };
    assert.commandWorked(testDB.runCommand(cmdObj));

    let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});

    if (!FeatureFlagUtil.isPresentAndEnabled(testDB, "QueryStatsUpdateCommand")) {
        assert.eq(queryStats, [], "expect no query stats for update when feature flag is off");
        return;
    }

    assert.eq(1, queryStats.length);
    assert.eq("update", queryStats[0].key.queryShape.command);
    assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);
    assert.eq("?object", queryStats[0].key.queryShape.u);
}

// TODO (SERVER-113907): Add tests for update with array filters.
//
// Modifier update tests.
//

// Helper to build mapping for simple operators that take a single value.
// By default, it assumes that the operator is applied to field "v" with a numeric value.
// If a different type needs to be used (i.e $currentDate), the caller can specify the expectedU,
// updateVal, and createCollection parameters for a different value type.
function modifierOperatorMapping({
    operator,
    expectedU = "?number",
    updateVal = Math.random(),
    createCollection = {v: Math.random()},
} = {}) {
    return {
        update: {[operator]: {v: updateVal}},
        insert: createCollection,
        expectedU: {[operator]: {[kHashedFieldName]: expectedU}},
    };
}

// Helper to build mapping for array operators.
// By default, it assumes that the operator is applied to field "v" with a numeric value in an array.
// If a different type needs to be used, the caller can specify the expectedU,
// updateVal, and createCollection parameters for a different value type.
function arrayModifierOperatorMapping({
    operator,
    updateVal = Math.random(),
    expectedU = "?number",
    createCollection = {v: [1]},
} = {}) {
    return modifierOperatorMapping({
        operator: operator,
        expectedU: expectedU,
        updateVal: updateVal,
        createCollection: createCollection,
    });
}

// Helper to build mapping for $push with $each.
// By default, it assumes that the operator is applied to field "v" with a numeric value in an array.
// Caller can specify additional operators to be included in the $push, such as $sort.
function arrayModifierOperatorMappingPushMultipleElements({
    additionalOperators = {},
    additionalOperatorsShapified = {},
} = {}) {
    return arrayModifierOperatorMapping({
        operator: "$push",
        updateVal: {...{$each: [Math.random()]}, ...additionalOperators},
        expectedU: {...{$each: "?array<?number>"}, ...additionalOperatorsShapified},
    });
}

function testModifierUpdateTokenizationForSetOfOperators(testDB, collName, coll, operatorTestCases) {
    // Iterate through each simple operator mapping and test.
    for (const {update, insert, expectedU} of operatorTestCases) {
        jsTest.log.info("Testing modifier update: " + tojson(update));
        coll.drop();
        assert.commandWorked(coll.insert(insert));

        const cmdObj = {
            update: collName,
            updates: [{q: {v: 1}, u: update}],
            comment: `modifier update for ${update}`,
        };
        assert.commandWorked(testDB.runCommand(cmdObj));

        let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});

        if (!FeatureFlagUtil.isPresentAndEnabled(testDB, "QueryStatsUpdateCommand")) {
            assert.eq(queryStats, [], "expect no query stats for update when feature flag is off");
            return;
        }

        assert.eq(1, queryStats.length);
        assert.eq("update", queryStats[0].key.queryShape.command);
        assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);
        assert.eq(expectedU, queryStats[0].key.queryShape.u);
        resetQueryStatsStore(testDB.getMongo(), "1MB");
    }
}

function testModifierUpdateTokenizationForSimpleOperators(testDB, collName, coll) {
    const simpleOperatorsMapping = [
        modifierOperatorMapping({operator: "$unset", expectedU: 1}),
        modifierOperatorMapping({operator: "$rename", expectedU: kHashedFieldForA, updateVal: "a"}),
        modifierOperatorMapping({
            operator: "$currentDate",
            expectedU: {$type: "timestamp"},
            updateVal: {$type: "timestamp"},
            createCollection: {v: ISODate("2013-10-02T01:11:18.965Z")},
        }),
    ];

    testModifierUpdateTokenizationForSetOfOperators(testDB, collName, coll, simpleOperatorsMapping);
}

function testModifierUpdateTokenizationForArithmeticOperators(testDB, collName, coll) {
    const arithmeticOperatorsMapping = [
        modifierOperatorMapping({operator: "$inc"}),
        modifierOperatorMapping({operator: "$min"}),
        modifierOperatorMapping({operator: "$max"}),
        modifierOperatorMapping({operator: "$mul"}),
        modifierOperatorMapping({
            operator: "$bit",
            expectedU: {and: "?number"},
            updateVal: {and: NumberInt(10)},
            createCollection: {v: NumberInt(1)},
        }),
    ];

    testModifierUpdateTokenizationForSetOfOperators(testDB, collName, coll, arithmeticOperatorsMapping);
}

function testModifierUpdateTokenizationForSimpleArrayOperators(testDB, collName, coll) {
    const arrayOperatorsMapping = [
        arrayModifierOperatorMapping({operator: "$addToSet"}),
        arrayModifierOperatorMapping({operator: "$pop", updateVal: -1, expectedU: -1}),
        arrayModifierOperatorMapping({
            operator: "$addToSet",
            updateVal: {$each: [5, 6, 7]},
            expectedU: {$each: "?array<?number>"},
        }),
    ];

    testModifierUpdateTokenizationForSetOfOperators(testDB, collName, coll, arrayOperatorsMapping);
}

// Test more complex modifier array operators like $push with additional operators and $pull with query filters.
function testModifierUpdateTokenizationForComplexArrayOperators(testDB, collName, coll) {
    const hashedFieldForQ = "S6zZh99V0NXwOj/n5vDnZLcS9d8Fwu1Qdz6ZNDu7++E="; // Hash of field "q"

    const arrayOperatorsMapping = [
        arrayModifierOperatorMappingPushMultipleElements(),
        arrayModifierOperatorMappingPushMultipleElements({
            additionalOperators: {$position: 0},
            additionalOperatorsShapified: {$position: "?number"},
        }),
        arrayModifierOperatorMappingPushMultipleElements({
            additionalOperators: {$slice: 1},
            additionalOperatorsShapified: {$slice: "?number"},
        }),
        arrayModifierOperatorMappingPushMultipleElements({
            additionalOperators: {$sort: {v: 1, a: -1}},
            additionalOperatorsShapified: {$sort: {[kHashedFieldName]: 1, [kHashedFieldForA]: -1}},
        }),

        // Pull operators and its variations.
        arrayModifierOperatorMapping({operator: "$pull"}),
        arrayModifierOperatorMapping({
            operator: "$pull",
            updateVal: {q: {$elemMatch: {a: {$gte: 8}}}},
            expectedU: {[hashedFieldForQ]: {$elemMatch: {[kHashedFieldForA]: {$gte: "?number"}}}},
        }),

        // Note that $options is empty in updateVal, hence why when serialized, it is stripped away in expectedU.
        arrayModifierOperatorMapping({
            operator: "$pull",
            updateVal: {"$regex": "(?i)a(?-i)bc", "$options": ""},
            expectedU: {"$regex": "?string"},
        }),
        arrayModifierOperatorMapping({operator: "$pullAll", updateVal: [4, 5, 6], expectedU: "?array<?number>"}),
    ];

    testModifierUpdateTokenizationForSetOfOperators(testDB, collName, coll, arrayOperatorsMapping);
}

//
// Pipeline update tests.
//

/**
 * Test with all possible allowed aggregation stages in a pipeline-style update:
 * - $addFields
 * - $project
 * - $replaceRoot
 */
function testPipelineUpdateTokenization(testDB, collName) {
    const cmdObj = {
        update: collName,
        updates: [
            {
                q: {v: 1},
                u: [{$addFields: {v: 2}}, {$project: {_id: 1}}, {$replaceRoot: {newRoot: {v: 3}}}],
            },
        ],
        comment: "pipeline update!",
    };
    assert.commandWorked(testDB.runCommand(cmdObj));

    let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});

    if (!FeatureFlagUtil.isPresentAndEnabled(testDB, "QueryStatsUpdateCommand")) {
        assert.eq(queryStats, [], "expect no query stats for update when feature flag is off");
        return;
    }

    assert.eq(1, queryStats.length);

    assert.eq("update", queryStats[0].key.queryShape.command);
    assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);

    const expectedU = [
        {$addFields: {[kHashedFieldName]: "?number"}},
        {$project: {"+0wgDp/AI7f+XT+DJEqixDyZBq9zRe7RGN0wCS9bd94=": true}},
        {$replaceRoot: {newRoot: "?object"}},
    ];
    assert.eq(expectedU, queryStats[0].key.queryShape.u);
}

/**
 * Test with the aliases of all possible allowed aggregation stages in a pipeline-style update.
 * - $set is an alias for $addFields
 * - $unset is an alias for $project
 * - $replaceWith is an alias for $replaceRoot
 */
function testPipelineUpdateWithAliasesTokenization(testDB, collName) {
    const cmdObj = {
        update: collName,
        updates: [
            {
                q: {v: 1},
                u: [{$set: {v: "newValue"}}, {$unset: "v"}, {$replaceWith: {newDoc: {v: 5}}}],
            },
        ],
        comment: "pipeline with stage aliases update!",
    };
    assert.commandWorked(testDB.runCommand(cmdObj));

    let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});

    if (!FeatureFlagUtil.isPresentAndEnabled(testDB, "QueryStatsUpdateCommand")) {
        assert.eq(queryStats, [], "expect no query stats for update when feature flag is off");
        return;
    }

    assert.eq(1, queryStats.length);
    assert.eq("update", queryStats[0].key.queryShape.command);
    assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);

    const expectedU = [
        {$set: {[kHashedFieldName]: "?string"}},
        {$project: {[kHashedFieldName]: false, [kHashedIdField]: true}}, // $unset aliases to exclusion $project.
        {$replaceRoot: {newRoot: "?object"}}, // $replaceWith aliases to $replaceRoot.
    ];
    assert.eq(expectedU, queryStats[0].key.queryShape.u);
}

/**
 * Test that a constant in a pipeline-style update is tokenized.
 */
function testPipelineUpdateWithConstantTokenization(testDB, collName) {
    const kHashedConstant = "$$VaksLvxpslthbM1qCItU0mhsNVyN8A97qAJzqIKrZoI="; // Hash of "$$newVal"
    const cmdObj = {
        update: collName,
        updates: [
            {
                q: {v: 1},
                u: [{$set: {v: "$$newVal"}}],
                c: {newVal: 500},
            },
        ],
        comment: "pipeline update with constants!",
    };
    assert.commandWorked(testDB.runCommand(cmdObj));

    let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});

    if (!FeatureFlagUtil.isPresentAndEnabled(testDB, "QueryStatsUpdateCommand")) {
        assert.eq(queryStats, [], "expect no query stats for update when feature flag is off");
        return;
    }

    assert.eq(1, queryStats.length);
    assert.eq("update", queryStats[0].key.queryShape.command);
    assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);
    assert.eq([{$set: {[kHashedFieldName]: kHashedConstant}}], queryStats[0].key.queryShape.u);
    assert.eq({newVal: "?number"}, queryStats[0].key.queryShape.c);
}

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();

    if (testDB.getMongo().isMongos()) {
        // TODO SERVER-112050 Unskip this when we support sharded clusters for update.
        jsTest.log.info("Skipping update tokenization test on sharded cluster");
        return;
    }

    coll.drop();
    assert.commandWorked(coll.insert({v: 1}));

    jsTest.log.info("Testing replacement update");
    testReplacementUpdateTokenization(testDB, collName);

    jsTest.log.info("Testing modifier update for simple operators");
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    testModifierUpdateTokenizationForSimpleOperators(testDB, collName, coll);

    jsTest.log.info("Testing modifier update for arithmetic operators");
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    testModifierUpdateTokenizationForArithmeticOperators(testDB, collName, coll);

    jsTest.log.info("Testing modifier update for simple array operators");
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    testModifierUpdateTokenizationForSimpleArrayOperators(testDB, collName, coll);

    jsTest.log.info("Testing modifier update for complex array operators");
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    testModifierUpdateTokenizationForComplexArrayOperators(testDB, collName, coll);

    jsTest.log.info("Testing pipeline update");
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    testPipelineUpdateTokenization(testDB, collName);

    jsTest.log.info("Testing pipeline update with aliases");
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    testPipelineUpdateWithAliasesTokenization(testDB, collName);

    jsTest.log.info("Testing pipeline update with constant");
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    testPipelineUpdateWithConstantTokenization(testDB, collName);
});
