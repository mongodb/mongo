/**
 * This test checks that search indexes cannot be created on view definitions that violate
 * constraints. See db/query/search/search_index_view_validation.h for more details.
 *
 * @tags: [ featureFlagExtensionsAPI ]
 */
import {isPlatformCompatibleWithExtensions} from "jstests/noPassthrough/libs/extension_helpers.js";

const invalidStageErrorCode = 10623000;
const matchErrorCode = 10623001;
const addFieldsErrorCode = 10623002;
const invalidOperatorErrorCode = 10623003;
const invalidVariableErrorCode = 10623004;
const overrideCurrentErrorCode = 10623005;

const testDb = db.getSiblingDB(jsTestName());
const coll = testDb.underlyingSourceCollection;
coll.drop();
coll.insertOne({});

const createViewAndSearchIndexDef = function (name, pipeline) {
    assert.commandWorked(testDb.createView(name, coll.getName(), pipeline));
    return {
        name: `${name}_index`,
        definition: {mappings: {dynamic: false, fields: {name: {type: "string"}}}},
    };
};

const testSearchIndexOnInvalidView = function ({name, pipeline, errorCode}) {
    const searchIndexDef = createViewAndSearchIndexDef(name, pipeline);
    assert.commandFailedWithCode(testDb.runCommand({createSearchIndexes: name, indexes: [searchIndexDef]}), errorCode);
};

// ===============================================================================
// Invalid stages.
// ===============================================================================
testSearchIndexOnInvalidView({
    name: "search_index_lookup_view",
    pipeline: [{$lookup: {from: coll.getName(), localField: "_id", foreignField: "abc", as: "123"}}],
    errorCode: invalidStageErrorCode,
});

testSearchIndexOnInvalidView({
    name: "search_index_unionWith_view",
    pipeline: [{$unionWith: {coll: coll.getName(), pipeline: []}}],
    errorCode: invalidStageErrorCode,
});

testSearchIndexOnInvalidView({
    name: "search_index_facet_view",
    pipeline: [{$facet: {"pipeline0": [{$match: {}}], "pipeline1": [{$match: {}}]}}],
    errorCode: invalidStageErrorCode,
});

testSearchIndexOnInvalidView({name: "search_index_search_view", pipeline: [{$search: {}}], errorCode: 10623000});

testSearchIndexOnInvalidView({
    name: "search_index_project_view",
    pipeline: [{$project: {abc: 1}}],
    errorCode: invalidStageErrorCode,
});

if (isPlatformCompatibleWithExtensions()) {
    testSearchIndexOnInvalidView({
        // Test with an extension that desugars into $addFields and $match (allowed stages).
        // This should fail as we do not desugar before making CRUD operations on search indexes,
        // meaning that the search index validator will see `$addFieldsMatch` (not supported)
        // rather than the desugared form of `$addFields` + `$match` (supported).
        name: "search_index_addFields_match_extension",
        pipeline: [
            {
                $addFieldsMatch: {
                    field: "apple",
                    value: "banana",
                    filter: "grape",
                },
            },
        ],
        errorCode: invalidStageErrorCode,
    });
}

// ===============================================================================
// Stage-specific constraints.
// ===============================================================================
testSearchIndexOnInvalidView({
    name: "search_index_match_without_expr_view",
    pipeline: [{$match: {abc: "123"}}],
    errorCode: matchErrorCode,
});

testSearchIndexOnInvalidView({
    name: "search_index_addFields_id_view",
    pipeline: [{$addFields: {_id: "new_id"}}],
    errorCode: addFieldsErrorCode,
});

// ===============================================================================
// Invalid operators.
// ===============================================================================
testSearchIndexOnInvalidView({
    name: "search_index_addFields_rand_view",
    pipeline: [{$addFields: {randomValue: {$rand: {}}}}],
    errorCode: invalidOperatorErrorCode,
});

testSearchIndexOnInvalidView({
    name: "search_index_addFields_function_view",
    pipeline: [
        {
            $addFields: {
                computedValue: {
                    $function: {
                        body: function (x) {
                            return x + 1;
                        },
                        args: ["$abc"],
                        lang: "js",
                    },
                },
            },
        },
    ],
    errorCode: invalidOperatorErrorCode,
});

// ===============================================================================
// Invalid variables.
// ===============================================================================
testSearchIndexOnInvalidView({
    name: "search_index_now_variable_view",
    pipeline: [{$addFields: {timestamp: "$$NOW"}}],
    errorCode: invalidVariableErrorCode,
});

testSearchIndexOnInvalidView({
    name: "search_index_cluster_time_variable_view",
    pipeline: [{$addFields: {timestamp: "$$CLUSTER_TIME"}}],
    errorCode: invalidVariableErrorCode,
});

testSearchIndexOnInvalidView({
    name: "search_index_user_roles_variable_view",
    pipeline: [{$addFields: {roles: "$$USER_ROLES"}}],
    errorCode: invalidVariableErrorCode,
});

testSearchIndexOnInvalidView({
    name: "search_index_let_override_current_view",
    pipeline: [{$addFields: {result: {$let: {vars: {CURRENT: "$name"}, in: "$$CURRENT"}}}}],
    errorCode: overrideCurrentErrorCode,
});

// ===============================================================================
// Valid: empty $match should be allowed.
// ===============================================================================
(function testEmptyMatchIsValid() {
    const viewName = "search_index_empty_match_view";
    const searchIndexDef = createViewAndSearchIndexDef(viewName, [{$match: {}}]);
    assert.commandWorked(testDb.runCommand({createSearchIndexes: viewName, indexes: [searchIndexDef]}));
})();
