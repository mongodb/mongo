/**
 * This test checks that search indexes cannot be created on view definitions that violate
 * constraints. See db/query/search/search_index_view_validation.h for more details.
 */
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

const testSearchIndexOnInvalidView = function({name, pipeline, errorCode}) {
    assert.commandWorked(testDb.createView(name, coll.getName(), pipeline));

    const searchIndexDef = {
        name: `${name}_index`,
        definition: {mappings: {dynamic: false, fields: {name: {type: "string"}}}}
    };

    assert.commandFailedWithCode(
        testDb.runCommand({createSearchIndexes: name, indexes: [searchIndexDef]}), errorCode);
};

// ===============================================================================
// Invalid stages.
// ===============================================================================
testSearchIndexOnInvalidView({
    name: "search_index_lookup_view",
    pipeline:
        [{$lookup: {from: coll.getName(), localField: "_id", foreignField: "abc", as: "123"}}],
    errorCode: invalidStageErrorCode
});

testSearchIndexOnInvalidView({
    name: "search_index_unionWith_view",
    pipeline: [{$unionWith: {coll: coll.getName(), pipeline: []}}],
    errorCode: invalidStageErrorCode
});

testSearchIndexOnInvalidView({
    name: "search_index_facet_view",
    pipeline: [{$facet: {"pipeline0": [{$match: {}}], "pipeline1": [{$match: {}}]}}],
    errorCode: invalidStageErrorCode
});

testSearchIndexOnInvalidView(
    {name: "search_index_search_view", pipeline: [{$search: {}}], errorCode: 10623000});

testSearchIndexOnInvalidView(
    {name: "search_index_project_view", pipeline: [{$project: {abc: 1}}], errorCode: 10623000});

// ===============================================================================
// Stage-specific constraints.
// ===============================================================================
testSearchIndexOnInvalidView({
    name: "search_index_match_without_expr_view",
    pipeline: [{$match: {abc: "123"}}],
    errorCode: matchErrorCode
});

testSearchIndexOnInvalidView({
    name: "search_index_addFields_id_view",
    pipeline: [{$addFields: {_id: "new_id"}}],
    errorCode: addFieldsErrorCode
});

// ===============================================================================
// Invalid operators.
// ===============================================================================
testSearchIndexOnInvalidView({
    name: "search_index_addFields_rand_view",
    pipeline: [{$addFields: {randomValue: {$rand: {}}}}],
    errorCode: invalidOperatorErrorCode
});

testSearchIndexOnInvalidView({
    name: "search_index_addFields_function_view",
    pipeline: [{
        $addFields: {
            computedValue: {
                $function: {
                    body: function(x) {
                        return x + 1;
                    },
                    args: ["$abc"],
                    lang: "js"
                }
            }
        }
    }],
    errorCode: invalidOperatorErrorCode
});

// ===============================================================================
// Invalid variables.
// ===============================================================================
testSearchIndexOnInvalidView({
    name: "search_index_now_variable_view",
    pipeline: [{$addFields: {timestamp: "$$NOW"}}],
    errorCode: invalidVariableErrorCode
});

testSearchIndexOnInvalidView({
    name: "search_index_cluster_time_variable_view",
    pipeline: [{$addFields: {timestamp: "$$CLUSTER_TIME"}}],
    errorCode: invalidVariableErrorCode
});

testSearchIndexOnInvalidView({
    name: "search_index_user_roles_variable_view",
    pipeline: [{$addFields: {roles: "$$USER_ROLES"}}],
    errorCode: invalidVariableErrorCode
});

testSearchIndexOnInvalidView({
    name: "search_index_let_override_current_view",
    pipeline: [{$addFields: {result: {$let: {vars: {CURRENT: "$name"}, in : "$$CURRENT"}}}}],
    errorCode: overrideCurrentErrorCode
});
