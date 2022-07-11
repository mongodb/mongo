/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/inclusion_projection_executor.h"

#include <vector>

#include "mongo/base/exact_cast.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::projection_executor {
namespace {
using std::vector;

template <typename T>
BSONObj wrapInLiteral(const T& arg) {
    return BSON("$literal" << arg);
}

/**
 * This test fixture run the test twice, one when the fast-path projection mode is allowed, another
 * one when it's not.
 *
 * The 'AllowFallBackToDefault' parameter should be set to 'true', if the executor is allowed to
 * fall back to the default inclusion projection implementation if the fast-path projection cannot
 * be used for a specific test. If set to 'false', an invariant will be triggered if fast-path
 * projection was expected to be chosen, but the default one has been picked instead.
 */
template <bool AllowFallBackToDefault>
class BaseInclusionProjectionExecutionTest : public mongo::unittest::Test {
public:
    void run() {
        auto base = static_cast<mongo::unittest::Test*>(this);
        try {
            if (_runFastPath) {
                _allowFastPath = true;
                base->run();
            }
            if (_runDefault) {
                _allowFastPath = false;
                base->run();
            }
        } catch (...) {
            LOGV2(20587,
                  "Exception while testing",
                  "allowFastPath"_attr = _allowFastPath,
                  "allowFallBackToDefault"_attr = AllowFallBackToDefault);
            throw;
        }
    }

protected:
    auto createProjectionExecutor(const BSONObj& projSpec,
                                  boost::optional<BSONObj> matchSpec,
                                  const ProjectionPolicies& policies) {
        const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        auto&& [projection, matchExpr] = [&] {
            if (matchSpec) {
                auto matchExpr = CopyableMatchExpression{*matchSpec, expCtx};
                return std::make_pair(
                    projection_ast::parseAndAnalyze(
                        expCtx, projSpec, &*matchExpr, matchExpr.inputBSON(), policies),
                    boost::make_optional(matchExpr));
            }
            return std::make_pair(projection_ast::parseAndAnalyze(expCtx, projSpec, policies),
                                  boost::optional<CopyableMatchExpression>{});
        }();

        auto builderParams{kDefaultBuilderParams};
        if (!_allowFastPath) {
            builderParams.reset(kAllowFastPath);
        }

        auto executor = buildProjectionExecutor(expCtx, &projection, policies, builderParams);
        invariant(executor->getType() ==
                  TransformerInterface::TransformerType::kInclusionProjection);
        auto inclusionExecutor = static_cast<InclusionProjectionExecutor*>(executor.get());
        auto fastPathRootNode =
            exact_pointer_cast<FastPathEligibleInclusionNode*>(inclusionExecutor->getRoot());
        if (_allowFastPath) {
            uassert(51752,
                    "Fast-path projection mode or fall back to default expected",
                    fastPathRootNode || AllowFallBackToDefault);
        } else {
            uassert(51753, "Default projection mode expected", !fastPathRootNode);
        }
        return std::make_pair(std::move(executor), matchExpr);
    }

    auto createProjectionExecutor(const BSONObj& spec, const ProjectionPolicies& policies) {
        auto&& [executor, matchExpr] = createProjectionExecutor(spec, boost::none, policies);
        return std::move(executor);
    }

    // Helper to simplify the creation of a InclusionProjectionExecutor with default policies.
    auto makeInclusionProjectionWithDefaultPolicies(BSONObj spec) {
        return createProjectionExecutor(spec, {});
    }

    // Helper to simplify the creation of a InclusionProjectionExecutor which excludes _id by
    // default.
    auto makeInclusionProjectionWithDefaultIdExclusion(BSONObj spec) {
        ProjectionPolicies defaultExcludeId{ProjectionPolicies::DefaultIdPolicy::kExcludeId,
                                            ProjectionPolicies::kArrayRecursionPolicyDefault,
                                            ProjectionPolicies::kComputedFieldsPolicyDefault};
        return createProjectionExecutor(spec, defaultExcludeId);
    }

    // Helper to simplify the creation of a InclusionProjectionExecutor which does not recurse
    // arrays.
    auto makeInclusionProjectionWithNoArrayRecursion(BSONObj spec) {
        ProjectionPolicies noArrayRecursion{
            ProjectionPolicies::kDefaultIdPolicyDefault,
            ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays,
            ProjectionPolicies::kComputedFieldsPolicyDefault};
        return createProjectionExecutor(spec, noArrayRecursion);
    }

    // Helper to simplify the creation of a InclusionProjectionExecutor with find-style projection
    // policies.
    auto makeInclusionProjectionWithFindPolicies(BSONObj projSpec, BSONObj matchSpec) {
        return createProjectionExecutor(
            projSpec, matchSpec, ProjectionPolicies::findProjectionPolicies());
    }

    // True, if the projection executor is allowed to use the fast-path inclusion projection
    // implementation.
    bool _allowFastPath{true};
    // Run the test using fast-path projection mode.
    bool _runFastPath{true};
    // Run the test using default projection mode.
    bool _runDefault{true};
};

using InclusionProjectionExecutionTestWithFallBackToDefault =
    BaseInclusionProjectionExecutionTest<true>;
using InclusionProjectionExecutionTestWithoutFallBackToDefault =
    BaseInclusionProjectionExecutionTest<false>;

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldAddIncludedFieldsToDependencies) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("_id" << false << "a" << true << "x.y" << true));

    DepsTracker deps;
    inclusion->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 2UL);
    ASSERT_EQ(deps.fields.count("_id"), 0UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
    ASSERT_EQ(deps.fields.count("x.y"), 1UL);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldAddIdToDependenciesIfNotSpecified) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a" << true));

    DepsTracker deps;
    inclusion->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 2UL);
    ASSERT_EQ(deps.fields.count("_id"), 1UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldAddDependenciesOfComputedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a"
                                                                     << "$a"
                                                                     << "x"
                                                                     << "$z"));

    DepsTracker deps;
    inclusion->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 3UL);
    ASSERT_EQ(deps.fields.count("_id"), 1UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
    ASSERT_EQ(deps.fields.count("z"), 1UL);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldAddPathToDependenciesForNestedComputedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("a.b.c" << BSON("$add" << BSON_ARRAY(1 << 2))));

    DepsTracker deps;
    inclusion->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 3UL);
    ASSERT_EQ(deps.fields.count("_id"), 1UL);
    ASSERT_EQ(deps.fields.count("a"), 1UL);
    ASSERT_EQ(deps.fields.count("a.b"), 1UL);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldNotAddTopLevelDependencyWithExpressionOnTopLevelPath) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(BSON("a" << BSON("$add" << BSON_ARRAY(1 << 2))));

    DepsTracker deps;
    inclusion->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 1UL);
    ASSERT_EQ(deps.fields.count("_id"), 1UL);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldAddPathToDependenciesForNestedComputedFieldsUsingVariableReferences) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("x.y"
                                                                     << "$z"));

    DepsTracker deps;
    inclusion->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 3UL);
    // Implicit "_id".
    ASSERT_EQ(deps.fields.count("_id"), 1UL);
    // Needed by the ExpressionFieldPath.
    ASSERT_EQ(deps.fields.count("z"), 1UL);
    // Needed to ensure we preserve the structure of the input document.
    ASSERT_EQ(deps.fields.count("x"), 1UL);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldSerializeToEquivalentProjection) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        fromjson("{a: {$add: ['$a', 2]}, b: {d: 3}, 'x.y': {$literal: 4}}"));

    // Adds implicit "_id" inclusion, converts numbers to bools, serializes expressions.
    auto expectedSerialization =
        Document(fromjson("{_id: true, a: {$add: [\"$a\", {$const: 2}]}, b: {d: true}, x: "
                          "{y: {$const: 4}}}"));

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion->serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(
        expectedSerialization,
        inclusion->serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion->serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(
        expectedSerialization,
        inclusion->serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldSerializeExplicitExclusionOfId) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(BSON("_id" << false << "a" << true));

    // Adds implicit "_id" inclusion, converts numbers to bools, serializes expressions.
    auto expectedSerialization = Document{{"a", true}, {"_id", false}};

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion->serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(
        expectedSerialization,
        inclusion->serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion->serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(
        expectedSerialization,
        inclusion->serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault, ShouldSerializeWithTopLevelID) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a" << 1 << "b" << 1));
    auto serialization = inclusion->serializeTransformation(boost::none);
    ASSERT_VALUE_EQ(serialization["a"], Value(true));
    ASSERT_VALUE_EQ(serialization["b"], Value(true));
    ASSERT_VALUE_EQ(serialization["_id"], Value(true));

    inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("a" << 1 << "b" << BSON("c" << 1 << "d" << 1)));
    serialization = inclusion->serializeTransformation(boost::none);
    ASSERT_VALUE_EQ(serialization["a"], Value(true));
    ASSERT_VALUE_EQ(serialization["b"]["c"], Value(true));
    ASSERT_VALUE_EQ(serialization["b"]["d"], Value(true));
    ASSERT_VALUE_EQ(serialization["_id"], Value(true));
    ASSERT_VALUE_EQ(serialization["b"]["_id"], Value());

    inclusion = makeInclusionProjectionWithDefaultIdExclusion(BSON("a" << true << "b" << true));
    serialization = inclusion->serializeTransformation(boost::none);
    ASSERT_VALUE_EQ(serialization["a"], Value(true));
    ASSERT_VALUE_EQ(serialization["b"], Value(true));
    ASSERT_VALUE_EQ(serialization["_id"], Value(false));

    inclusion = makeInclusionProjectionWithDefaultIdExclusion(
        BSON("a" << true << "b" << true << "_id" << false));
    serialization = inclusion->serializeTransformation(boost::none);
    ASSERT_VALUE_EQ(serialization["a"], Value(true));
    ASSERT_VALUE_EQ(serialization["b"], Value(true));
    ASSERT_VALUE_EQ(serialization["_id"], Value(false));

    inclusion = makeInclusionProjectionWithDefaultIdExclusion(
        BSON("a" << true << "b" << true << "_id" << true));
    serialization = inclusion->serializeTransformation(boost::none);
    ASSERT_VALUE_EQ(serialization["a"], Value(true));
    ASSERT_VALUE_EQ(serialization["b"], Value(true));
    ASSERT_VALUE_EQ(serialization["_id"], Value(true));
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault, ShouldOptimizeTopLevelExpressions) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(BSON("a" << BSON("$add" << BSON_ARRAY(1 << 2))));

    inclusion->optimize();

    auto expectedSerialization = Document{{"_id", true}, {"a", Document{{"$const", 3}}}};

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion->serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(
        expectedSerialization,
        inclusion->serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion->serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(
        expectedSerialization,
        inclusion->serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault, ShouldOptimizeNestedExpressions) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("a.b" << BSON("$add" << BSON_ARRAY(1 << 2))));

    inclusion->optimize();

    auto expectedSerialization =
        Document{{"_id", true}, {"a", Document{{"b", Document{{"$const", 3}}}}}};

    // Should be the same if we're serializing for explain or for internal use.
    ASSERT_DOCUMENT_EQ(expectedSerialization, inclusion->serializeTransformation(boost::none));
    ASSERT_DOCUMENT_EQ(
        expectedSerialization,
        inclusion->serializeTransformation(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_DOCUMENT_EQ(expectedSerialization,
                       inclusion->serializeTransformation(ExplainOptions::Verbosity::kExecStats));
    ASSERT_DOCUMENT_EQ(
        expectedSerialization,
        inclusion->serializeTransformation(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldReportThatAllExceptIncludedFieldsAreModified) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("a" << wrapInLiteral("computedVal") << "b.c" << wrapInLiteral("computedVal") << "d"
                 << true << "e.f" << true));

    auto modifiedPaths = inclusion->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    // Included paths are not modified.
    ASSERT_EQ(modifiedPaths.paths.count("_id"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("d"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("e.f"), 1UL);
    // Computed paths are modified.
    ASSERT_EQ(modifiedPaths.paths.count("a"), 0UL);
    ASSERT_EQ(modifiedPaths.paths.count("b.c"), 0UL);
    ASSERT_EQ(modifiedPaths.paths.size(), 3UL);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldReportThatAllExceptIncludedFieldsAreModifiedWithIdExclusion) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("_id" << false << "a" << wrapInLiteral("computedVal") << "b.c"
                   << wrapInLiteral("computedVal") << "d" << true << "e.f" << true));

    auto modifiedPaths = inclusion->getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kAllExcept);
    // Included paths are not modified.
    ASSERT_EQ(modifiedPaths.paths.count("d"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("e.f"), 1UL);
    // Computed paths are modified.
    ASSERT_EQ(modifiedPaths.paths.count("a"), 0UL);
    ASSERT_EQ(modifiedPaths.paths.count("b.c"), 0UL);
    // _id is explicitly excluded.
    ASSERT_EQ(modifiedPaths.paths.count("_id"), 0UL);

    ASSERT_EQ(modifiedPaths.paths.size(), 2UL);
}

//
// Top-level only.
//

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault, ShouldIncludeTopLevelField) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a" << true));

    // More than one field in document.
    auto result = inclusion->applyTransformation(Document{{"a", 1}, {"b", 2}});
    auto expectedResult = Document{{"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the document.
    result = inclusion->applyTransformation(Document{{"a", 1}});
    expectedResult = Document{{"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the document.
    result = inclusion->applyTransformation(Document{{"c", 1}});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in the document.
    result = inclusion->applyTransformation(Document{});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault, ShouldAddComputedTopLevelField) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("newField" << wrapInLiteral("computedVal")));
    auto result = inclusion->applyTransformation(Document{});
    auto expectedResult = Document{{"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Computed field should replace existing field.
    result = inclusion->applyTransformation(Document{{"newField", "preExisting"_sd}});
    expectedResult = Document{{"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldApplyBothInclusionsAndComputedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("a" << true << "newField" << wrapInLiteral("computedVal")));
    auto result = inclusion->applyTransformation(Document{{"a", 1}});
    auto expectedResult = Document{{"a", 1}, {"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldIncludeFieldsInOrderOfInputDoc) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("first" << true << "second" << true << "third" << true));
    auto inputDoc = Document{{"second", 1}, {"first", 0}, {"third", 2}};
    auto result = inclusion->applyTransformation(inputDoc);
    ASSERT_DOCUMENT_EQ(result, inputDoc);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldApplyComputedFieldsInOrderSpecified) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON(
        "firstComputed" << wrapInLiteral("FIRST") << "secondComputed" << wrapInLiteral("SECOND")));
    auto result =
        inclusion->applyTransformation(Document{{"first", 0}, {"second", 1}, {"third", 2}});
    auto expectedResult = Document{{"firstComputed", "FIRST"_sd}, {"secondComputed", "SECOND"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault, ShouldImplicitlyIncludeId) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a" << true));
    auto result = inclusion->applyTransformation(Document{{"_id", "ID"_sd}, {"a", 1}, {"b", 2}});
    auto expectedResult = Document{{"_id", "ID"_sd}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should leave the "_id" in the same place as in the original document.
    result = inclusion->applyTransformation(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    expectedResult = Document{{"a", 1}, {"_id", "ID"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldImplicitlyIncludeIdWithComputedFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("newField" << wrapInLiteral("computedVal")));
    auto result = inclusion->applyTransformation(Document{{"_id", "ID"_sd}, {"a", 1}});
    auto expectedResult = Document{{"_id", "ID"_sd}, {"newField", "computedVal"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldIncludeIdIfExplicitlyIncluded) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("a" << true << "_id" << true << "b" << true));
    auto result =
        inclusion->applyTransformation(Document{{"_id", "ID"_sd}, {"a", 1}, {"b", 2}, {"c", 3}});
    auto expectedResult = Document{{"_id", "ID"_sd}, {"a", 1}, {"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldExcludeIdIfExplicitlyExcluded) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(BSON("a" << true << "_id" << false));
    auto result = inclusion->applyTransformation(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault, ShouldReplaceIdWithComputedId) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(BSON("_id" << wrapInLiteral("newId")));
    auto result = inclusion->applyTransformation(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"_id", "newId"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Projections with nested fields.
//

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldIncludeSimpleDottedFieldFromSubDoc) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a.b" << true));

    // More than one field in sub document.
    auto result = inclusion->applyTransformation(Document{{"a", Document{{"b", 1}, {"c", 2}}}});
    auto expectedResult = Document{{"a", Document{{"b", 1}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the sub document.
    result = inclusion->applyTransformation(Document{{"a", Document{{"b", 1}}}});
    expectedResult = Document{{"a", Document{{"b", 1}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the sub document.
    result = inclusion->applyTransformation(Document{{"a", Document{{"c", 1}}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in sub document.
    result = inclusion->applyTransformation(Document{{"a", Document{}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldNotCreateSubDocIfDottedIncludedFieldDoesNotExist) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("sub.target" << true));

    // Should not add the path if it doesn't exist.
    auto result = inclusion->applyTransformation(Document{});
    auto expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should not replace the first part of the path if that part exists.
    result = inclusion->applyTransformation(Document{{"sub", "notADocument"_sd}});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldApplyDottedInclusionToEachElementInArray) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a.b" << true));

    // Drops non-documents and non-arrays. Applies projection to documents, recurses on
    // nested arrays.
    auto result = inclusion->applyTransformation(Document{{"a",
                                                           {1,
                                                            Document{},
                                                            Document{{"b", 1}},
                                                            Document{{"b", 1}, {"c", 2}},
                                                            vector<Value>{},
                                                            {1, Document{{"c", 1}}}}}});
    auto expectedResult = Document{{"a",
                                    {Value(),
                                     Document{},
                                     Document{{"b", 1}},
                                     Document{{"b", 1}},
                                     vector<Value>{},
                                     {Value(), Document{}}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldAddComputedDottedFieldToSubDocument) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("sub.target" << wrapInLiteral("computedVal")));

    // Other fields exist in sub document, one of which is the specified field.
    auto result =
        inclusion->applyTransformation(Document{{"sub", Document{{"target", 1}, {"c", 2}}}});
    auto expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the sub document.
    result = inclusion->applyTransformation(Document{{"sub", Document{{"c", 1}}}});
    expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in sub document.
    result = inclusion->applyTransformation(Document{{"sub", Document{}}});
    expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldCreateSubDocIfDottedComputedFieldDoesntExist) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("sub.target" << wrapInLiteral("computedVal")));

    // Should add the path if it doesn't exist.
    auto result = inclusion->applyTransformation(Document{});
    auto expectedResult = Document{{"sub", Document{{"target", "computedVal"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should replace non-documents with documents.
    result = inclusion->applyTransformation(Document{{"sub", "notADocument"_sd}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldCreateNestedSubDocumentsAllTheWayToComputedField) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(BSON("a.b.c.d" << wrapInLiteral("computedVal")));

    // Should add the path if it doesn't exist.
    auto result = inclusion->applyTransformation(Document{});
    auto expectedResult =
        Document{{"a", Document{{"b", Document{{"c", Document{{"d", "computedVal"_sd}}}}}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should replace non-documents with documents.
    result = inclusion->applyTransformation(Document{{"a", Document{{"b", "other"_sd}}}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldAddComputedDottedFieldToEachElementInArray) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(BSON("a.b" << wrapInLiteral("COMPUTED")));

    auto result = inclusion->applyTransformation(Document{{"a",
                                                           {1,
                                                            Document{},
                                                            Document{{"b", 1}},
                                                            Document{{"b", 1}, {"c", 2}},
                                                            vector<Value>{},
                                                            {1, Document{{"c", 1}}}}}});
    auto expectedResult =
        Document{{"a",
                  {Document{{"b", "COMPUTED"_sd}},
                   Document{{"b", "COMPUTED"_sd}},
                   Document{{"b", "COMPUTED"_sd}},
                   Document{{"b", "COMPUTED"_sd}},
                   vector<Value>{},
                   {Document{{"b", "COMPUTED"_sd}}, Document{{"b", "COMPUTED"_sd}}}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldApplyInclusionsAndAdditionsToEachElementInArray) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("a.inc" << true << "a.comp" << wrapInLiteral("COMPUTED")));

    auto result = inclusion->applyTransformation(
        Document{{"a",
                  {1,
                   Document{},
                   Document{{"inc", 1}},
                   Document{{"inc", 1}, {"c", 2}},
                   Document{{"c", 2}, {"inc", 1}},
                   Document{{"inc", 1}, {"c", 2}, {"comp", "original"_sd}},
                   vector<Value>{},
                   {1, Document{{"inc", 1}}}}}});
    auto expectedResult = Document{
        {"a",
         {Document{{"comp", "COMPUTED"_sd}},
          Document{{"comp", "COMPUTED"_sd}},
          Document{{"inc", 1}, {"comp", "COMPUTED"_sd}},
          Document{{"inc", 1}, {"comp", "COMPUTED"_sd}},
          Document{{"inc", 1}, {"comp", "COMPUTED"_sd}},
          Document{{"inc", 1}, {"comp", "COMPUTED"_sd}},
          vector<Value>{},
          {Document{{"comp", "COMPUTED"_sd}}, Document{{"inc", 1}, {"comp", "COMPUTED"_sd}}}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault, ShouldAddOrIncludeSubFieldsOfId) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("_id.X" << true << "_id.Z" << wrapInLiteral("NEW")));
    auto result = inclusion->applyTransformation(Document{{"_id", Document{{"X", 1}, {"Y", 2}}}});
    auto expectedResult = Document{{"_id", Document{{"X", 1}, {"Z", "NEW"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldAllowMixedNestedAndDottedFields) {
    // Include all of "a.b", "a.c", "a.d", and "a.e".
    // Add new computed fields "a.W", "a.X", "a.Y", and "a.Z".
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("a.b" << true << "a.c" << true << "a.W" << wrapInLiteral("W") << "a.X"
                   << wrapInLiteral("X") << "a"
                   << BSON("d" << true << "e" << true << "Y" << wrapInLiteral("Y") << "Z"
                               << wrapInLiteral("Z"))));
    auto result = inclusion->applyTransformation(Document{
        {"a",
         Document{{"b", "b"_sd}, {"c", "c"_sd}, {"d", "d"_sd}, {"e", "e"_sd}, {"f", "f"_sd}}}});
    auto expectedResult = Document{{"a",
                                    Document{{"b", "b"_sd},
                                             {"c", "c"_sd},
                                             {"d", "d"_sd},
                                             {"e", "e"_sd},
                                             {"W", "W"_sd},
                                             {"X", "X"_sd},
                                             {"Y", "Y"_sd},
                                             {"Z", "Z"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldApplyNestedComputedFieldsInOrderSpecified) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("a" << wrapInLiteral("FIRST") << "b.c" << wrapInLiteral("SECOND")));
    auto result = inclusion->applyTransformation(Document{});
    auto expectedResult = Document{{"a", "FIRST"_sd}, {"b", Document{{"c", "SECOND"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldApplyComputedFieldsAfterAllInclusions) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("b.c" << wrapInLiteral("NEW") << "a" << true));
    auto result = inclusion->applyTransformation(Document{{"a", 1}});
    auto expectedResult = Document{{"a", 1}, {"b", Document{{"c", "NEW"_sd}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    result = inclusion->applyTransformation(Document{{"a", 1}, {"b", 4}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // In this case, the field 'b' shows up first and has a nested inclusion or computed
    // field. Even though it is a computed field, it will appear first in the output
    // document. This is inconsistent, but the expected behavior, and a consequence of
    // applying the projection recursively to each sub-document.
    result = inclusion->applyTransformation(Document{{"b", 4}, {"a", 1}});
    expectedResult = Document{{"b", Document{{"c", "NEW"_sd}}}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ComputedFieldReplacingExistingShouldAppearAfterInclusions) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("b" << wrapInLiteral("NEW") << "a" << true));
    auto result = inclusion->applyTransformation(Document{{"b", 1}, {"a", 1}});
    auto expectedResult = Document{{"a", 1}, {"b", "NEW"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    result = inclusion->applyTransformation(Document{{"a", 1}, {"b", 4}});
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Metadata inclusion.
//

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldAlwaysKeepMetadataFromOriginalDoc) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a" << true));

    MutableDocument inputDocBuilder(Document{{"a", 1}});
    inputDocBuilder.metadata().setRandVal(1.0);
    inputDocBuilder.metadata().setTextScore(10.0);
    Document inputDoc = inputDocBuilder.freeze();

    auto result = inclusion->applyTransformation(inputDoc);

    MutableDocument expectedDoc(inputDoc);
    expectedDoc.copyMetaDataFrom(inputDoc);
    ASSERT_DOCUMENT_EQ(result, expectedDoc.freeze());
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ShouldAddMetaExpressionsToDependencies) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(fromjson("{a: 1, c: {$meta: 'textScore'}, "
                                                            "d: {$meta: 'randVal'}, "
                                                            "e: {$meta: 'searchScore'}, "
                                                            "f: {$meta: 'searchHighlights'}, "
                                                            "g: {$meta: 'geoNearDistance'}, "
                                                            "h: {$meta: 'geoNearPoint'}, "
                                                            "i: {$meta: 'recordId'}, "
                                                            "j: {$meta: 'indexKey'}, "
                                                            "k: {$meta: 'sortKey'}, "
                                                            "l: {$meta: 'searchScoreDetails'}}"));

    DepsTracker deps;
    inclusion->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 2UL);

    // We do not add the dependencies for searchScore, searchHighlights, or searchScoreDetails
    // because those values are not stored in the collection (or in mongod at all).
    ASSERT_FALSE(deps.metadataDeps()[DocumentMetadataFields::kSearchScore]);
    ASSERT_FALSE(deps.metadataDeps()[DocumentMetadataFields::kSearchHighlights]);
    ASSERT_FALSE(deps.metadataDeps()[DocumentMetadataFields::kSearchScoreDetails]);

    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kTextScore]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kRandVal]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kGeoNearDist]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kGeoNearPoint]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kRecordId]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kIndexKey]);
    ASSERT_TRUE(deps.metadataDeps()[DocumentMetadataFields::kSortKey]);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault, ShouldEvaluateMetaExpressions) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(fromjson("{a: 1, c: {$meta: 'textScore'}, "
                                                            "d: {$meta: 'randVal'}, "
                                                            "e: {$meta: 'searchScore'}, "
                                                            "f: {$meta: 'searchHighlights'}, "
                                                            "g: {$meta: 'geoNearDistance'}, "
                                                            "h: {$meta: 'geoNearPoint'}, "
                                                            "i: {$meta: 'recordId'}, "
                                                            "j: {$meta: 'indexKey'}, "
                                                            "k: {$meta: 'sortKey'}, "
                                                            "l: {$meta: 'searchScoreDetails'}}"));

    MutableDocument inputDocBuilder(Document{{"a", 1}});
    inputDocBuilder.metadata().setTextScore(0.0);
    inputDocBuilder.metadata().setRandVal(1.0);
    inputDocBuilder.metadata().setSearchScore(2.0);
    inputDocBuilder.metadata().setSearchHighlights(Value{"foo"_sd});
    inputDocBuilder.metadata().setGeoNearDistance(3.0);
    inputDocBuilder.metadata().setGeoNearPoint(Value{BSON_ARRAY(4 << 5)});
    inputDocBuilder.metadata().setRecordId(RecordId{6});
    inputDocBuilder.metadata().setIndexKey(BSON("foo" << 7));
    inputDocBuilder.metadata().setSortKey(Value{Document{{"bar", 8}}}, true);
    inputDocBuilder.metadata().setSearchScoreDetails(BSON("scoreDetails"
                                                          << "foo"));
    Document inputDoc = inputDocBuilder.freeze();

    auto result = inclusion->applyTransformation(inputDoc);

    ASSERT_DOCUMENT_EQ(result,
                       Document{fromjson("{a: 1, c: 0.0, d: 1.0, e: 2.0, f: 'foo', g: 3.0, "
                                         "h: [4, 5], i: 6, j: {foo: 7}, k: [{bar: 8}], "
                                         "l: {scoreDetails: 'foo'}}")});
}

//
// _id inclusion policy.
//

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault, ShouldIncludeIdByDefault) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a" << true));

    auto result = inclusion->applyTransformation(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"_id", 2}, {"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault, ShouldIncludeIdWithIncludePolicy) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a" << true));

    auto result = inclusion->applyTransformation(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"_id", 2}, {"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault, ShouldExcludeIdWithExcludePolicy) {
    auto inclusion = makeInclusionProjectionWithDefaultIdExclusion(BSON("a" << true));

    auto result = inclusion->applyTransformation(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldOverrideIncludePolicyWithExplicitExcludeIdSpec) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(BSON("_id" << false << "a" << true));

    auto result = inclusion->applyTransformation(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldOverrideExcludePolicyWithExplicitIncludeIdSpec) {
    auto inclusion =
        makeInclusionProjectionWithDefaultIdExclusion(BSON("_id" << true << "a" << true));

    auto result = inclusion->applyTransformation(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"_id", 2}, {"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldAllowInclusionOfIdSubfieldWithDefaultIncludePolicy) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(BSON("_id.id1" << true << "a" << true));

    auto result = inclusion->applyTransformation(
        Document{{"_id", Document{{"id1", 1}, {"id2", 2}}}, {"a", 3}, {"b", 4}});
    auto expectedResult = Document{{"_id", Document{{"id1", 1}}}, {"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldAllowInclusionOfIdSubfieldWithDefaultExcludePolicy) {
    auto inclusion =
        makeInclusionProjectionWithDefaultIdExclusion(BSON("_id.id1" << true << "a" << true));

    auto result = inclusion->applyTransformation(
        Document{{"_id", Document{{"id1", 1}, {"id2", 2}}}, {"a", 3}, {"b", 4}});
    auto expectedResult = Document{{"_id", Document{{"id1", 1}}}, {"a", 3}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Nested array recursion.
//

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldRecurseNestedArraysByDefault) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a.b" << true));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => {a: [{b: 2}, [{b: 4}], {}]}
    auto result = inclusion->applyTransformation(Document{
        {"a",
         {1, Document{{"b", 2}, {"c", 3}}, {Document{{"b", 4}, {"c", 5}}}, Document{{"d", 6}}}}});

    auto expectedResult =
        Document{{"a", {Value(), Document{{"b", 2}}, {Document{{"b", 4}}}, Document{}}}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldNotRecurseNestedArraysForNoRecursePolicy) {
    auto inclusion = makeInclusionProjectionWithNoArrayRecursion(BSON("a.b" << true));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => {a: [{b: 2}, {}]}
    auto result = inclusion->applyTransformation(Document{
        {"a",
         {1, Document{{"b", 2}, {"c", 3}}, {Document{{"b", 4}, {"c", 5}}}, Document{{"d", 6}}}}});

    auto expectedResult = Document{{"a", {Value(), Document{{"b", 2}}, Value(), Document{}}}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       ShouldRetainNestedArraysIfNoRecursionNeeded) {
    auto inclusion = makeInclusionProjectionWithNoArrayRecursion(BSON("a" << true));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => [output doc identical to input]
    const auto inputDoc = Document{
        {"a",
         {1, Document{{"b", 2}, {"c", 3}}, {Document{{"b", 4}, {"c", 5}}}, Document{{"d", 6}}}}};

    auto result = inclusion->applyTransformation(inputDoc);
    const auto& expectedResult = inputDoc;

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ComputedFieldIsAddedToNestedArrayElementsForRecursePolicy) {
    auto inclusion =
        makeInclusionProjectionWithDefaultPolicies(BSON("a.b" << wrapInLiteral("COMPUTED")));

    auto result = inclusion->applyTransformation(Document{{"a",
                                                           {1,
                                                            Document{},
                                                            Document{{"b", 1}},
                                                            Document{{"b", 1}, {"c", 2}},
                                                            vector<Value>{},
                                                            {1, Document{{"c", 1}}}}}});
    auto expectedResult =
        Document{{"a",
                  {Document{{"b", "COMPUTED"_sd}},
                   Document{{"b", "COMPUTED"_sd}},
                   Document{{"b", "COMPUTED"_sd}},
                   Document{{"b", "COMPUTED"_sd}},
                   vector<Value>{},
                   {Document{{"b", "COMPUTED"_sd}}, Document{{"b", "COMPUTED"_sd}}}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ComputedFieldShouldReplaceNestedArrayForNoRecursePolicy) {
    auto inclusion =
        makeInclusionProjectionWithNoArrayRecursion(BSON("a.b" << wrapInLiteral("COMPUTED")));

    // For kRecurseNestedArrays, the computed field (1) replaces any scalar values in the
    // array with a subdocument containing the new field, and (2) is added to each element
    // of the array and all nested arrays individually. With kDoNotRecurseNestedArrays, the
    // nested arrays are replaced rather than being traversed, in exactly the same way as
    // scalar values.
    auto result = inclusion->applyTransformation(Document{{"a",
                                                           {1,
                                                            Document{},
                                                            Document{{"b", 1}},
                                                            Document{{"b", 1}, {"c", 2}},
                                                            vector<Value>{},
                                                            {1, Document{{"c", 1}}}}}});
    auto expectedResult = Document{{"a",
                                    {Document{{"b", "COMPUTED"_sd}},
                                     Document{{"b", "COMPUTED"_sd}},
                                     Document{{"b", "COMPUTED"_sd}},
                                     Document{{"b", "COMPUTED"_sd}},
                                     Document{{"b", "COMPUTED"_sd}},
                                     Document{{"b", "COMPUTED"_sd}}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault, ExtractComputedProjections) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON(
        "computedMeta1" << BSON("$toUpper"
                                << "$myMeta.x")
                        << "computed2" << BSON("$add" << BSON_ARRAY(1 << "$c")) << "computedMeta3"
                        << "$myMeta"));

    auto r = static_cast<InclusionProjectionExecutor*>(inclusion.get())->getRoot();
    const std::set<StringData> reservedNames{};
    auto [addFields, deleteFlag] =
        r->extractComputedProjectionsInProject("myMeta", "meta", reservedNames);

    ASSERT_EQ(addFields.nFields(), 2);
    auto expectedAddFields =
        BSON("computedMeta1" << BSON("$toUpper" << BSON_ARRAY("$meta.x")) << "computedMeta3"
                             << "$meta");
    ASSERT_BSONOBJ_EQ(expectedAddFields, addFields);
    ASSERT_EQ(deleteFlag, false);

    auto expectedProjection =
        Document(fromjson("{_id: true, computedMeta1: true, computed2: {$add: [{$const: "
                          "1}, \"$c\"]}, computedMeta3: \"$computedMeta3\"}"));
    ASSERT_DOCUMENT_EQ(expectedProjection, inclusion->serializeTransformation(boost::none));
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault,
       ExtractComputedProjectionInProjectShouldNotHideDependentFields) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a"
                                                                     << "$myMeta"
                                                                     << "b"
                                                                     << "$a"));

    auto r = static_cast<InclusionProjectionExecutor*>(inclusion.get())->getRoot();
    const std::set<StringData> reservedNames{};
    auto [addFields, deleteFlag] =
        r->extractComputedProjectionsInProject("myMeta", "meta", reservedNames);

    ASSERT_EQ(addFields.nFields(), 0);
    ASSERT_EQ(deleteFlag, false);

    auto expectedProjection = Document(fromjson("{_id: true, a: '$myMeta', b: '$a'}"));
    ASSERT_DOCUMENT_EQ(expectedProjection, inclusion->serializeTransformation(boost::none));
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault, ApplyProjectionAfterSplit) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(
        BSON("a" << true << "computedMeta1"
                 << BSON("$toUpper"
                         << "$myMeta.x")
                 << "computed2" << BSON("$add" << BSON_ARRAY(1 << "$c")) << "c" << true
                 << "computedMeta3"
                 << "$myMeta"));

    auto r = static_cast<InclusionProjectionExecutor*>(inclusion.get())->getRoot();
    const std::set<StringData> reservedNames{};
    auto [addFields, deleteFlag] =
        r->extractComputedProjectionsInProject("myMeta", "meta", reservedNames);

    // Assuming the document was produced by the $_internalUnpackBucket.
    auto result = inclusion->applyTransformation(
        Document{{"a", 1}, {"c", 5}, {"computedMeta1", "XXX"_sd}, {"computedMeta3", 2}});
    // Computed projections preserve the order in $project, field 'c' moves in front of them.
    auto expectedResult = Document{
        {"a", 1}, {"c", 5}, {"computedMeta1", "XXX"_sd}, {"computed2", 6}, {"computedMeta3", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST_F(InclusionProjectionExecutionTestWithFallBackToDefault, DoNotExtractReservedNames) {
    auto inclusion = makeInclusionProjectionWithDefaultPolicies(BSON("a" << true << "data"
                                                                         << BSON("$toUpper"
                                                                                 << "$myMeta.x")
                                                                         << "newMeta"
                                                                         << "$myMeta"));

    auto r = static_cast<InclusionProjectionExecutor*>(inclusion.get())->getRoot();
    const std::set<StringData> reservedNames{"meta", "data", "_id"};
    auto [addFields, deleteFlag] =
        r->extractComputedProjectionsInProject("myMeta", "meta", reservedNames);

    ASSERT_EQ(addFields.nFields(), 1);
    auto expectedAddFields = BSON("newMeta"
                                  << "$meta");
    ASSERT_BSONOBJ_EQ(expectedAddFields, addFields);
    ASSERT_EQ(deleteFlag, false);

    auto expectedProjection = Document(fromjson(
        "{_id: true, a: true, data: {\"$toUpper\" : [\"$myMeta.x\"]}, newMeta: \"$newMeta\"}"));
    ASSERT_DOCUMENT_EQ(expectedProjection, inclusion->serializeTransformation(boost::none));
}
}  // namespace

// The tests in this block are for the fast-path projection only, as the default projection mode
// would always succeed, so we'll set the _runDefault flag to false to skip applying the projection
// in default mode.
namespace fast_path_projection_only_tests {
TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       CannotUseFastPathWithFindPositional) {
    _runDefault = false;
    ASSERT_THROWS_CODE(
        makeInclusionProjectionWithFindPolicies(fromjson("{a: 1, 'b.$': 1}"), fromjson("{b: 1}")),
        AssertionException,
        51752);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault, CannotUseFastPathWithFindSlice) {
    _runDefault = false;
    ASSERT_THROWS_CODE(
        makeInclusionProjectionWithFindPolicies(fromjson("{a: 1, b: {$slice: 2}}"), {}),
        AssertionException,
        51752);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       CannotUseFastPathWithFindElemMatch) {
    _runDefault = false;
    ASSERT_THROWS_CODE(
        makeInclusionProjectionWithFindPolicies(fromjson("{a: 1, b: {$elemMatch: {c: 1}}}"), {}),
        AssertionException,
        51752);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       CannotUseFastPathWithRegularExpression) {
    _runDefault = false;
    ASSERT_THROWS_CODE(
        makeInclusionProjectionWithDefaultPolicies(fromjson("{a: 1, b: {$add: ['$c', 1]}}")),
        AssertionException,
        51752);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       CannotUseFastPathWithMetadataExpression) {
    _runDefault = false;
    ASSERT_THROWS_CODE(
        makeInclusionProjectionWithDefaultPolicies(fromjson("{a: 1, b: {$meta: 'randVal'}}")),
        AssertionException,
        51752);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault, CannotUseFastPathWithLiteral) {
    _runDefault = false;
    ASSERT_THROWS_CODE(
        makeInclusionProjectionWithDefaultPolicies(BSON("a" << 1 << "b" << wrapInLiteral("abc"))),
        AssertionException,
        51752);
}

TEST_F(InclusionProjectionExecutionTestWithoutFallBackToDefault,
       CannotUseFastPathWithFieldPathExpression) {
    _runDefault = false;
    ASSERT_THROWS_CODE(makeInclusionProjectionWithDefaultPolicies(fromjson("{a: 1, b: '$c'}")),
                       AssertionException,
                       51752);
}

}  // namespace fast_path_projection_only_tests
}  // namespace mongo::projection_executor
