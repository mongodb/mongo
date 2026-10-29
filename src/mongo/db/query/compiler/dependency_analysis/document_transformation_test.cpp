// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/dependency_analysis/document_transformation.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/document_transformation_helpers.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::document_transformation {
namespace {
using namespace std::literals::string_view_literals;

using namespace mongo::unittest::match;
using namespace std::string_literals;

TEST(DocumentTransformationTest, ModifyPathDefaults) {
    ModifyPath op{"a.b.c", ModifiedPrefixPolicy::kNotSupported};
    EXPECT_EQ(op.getPath(), "a.b.c"sv);
    EXPECT_TRUE(op.isComputed());
    EXPECT_FALSE(op.isRemoved());
    EXPECT_EQ(op.getExpression(), nullptr);
    EXPECT_EQ(op.getPrefixPolicy(), ModifiedPrefixPolicy::kNotSupported);
}

TEST(DocumentTransformationTest, SimpleRenamePathDefaults) {
    RenamePath op{"a", "x"};
    EXPECT_EQ(op.getNewPath(), "a"sv);
    EXPECT_EQ(op.getOldPath(), "x"sv);
    EXPECT_EQ(op.getNewPathMaxArrayTraversals(), 0);
    EXPECT_EQ(op.getOldPathMaxArrayTraversals(), 0);
}

TEST(DocumentTransformationTest, ComplexRenamePathDefaults) {
    RenamePath op{"a", "x.y"};
    EXPECT_EQ(op.getNewPath(), "a"sv);
    EXPECT_EQ(op.getOldPath(), "x.y"sv);
    EXPECT_EQ(op.getNewPathMaxArrayTraversals(), 0);
    EXPECT_EQ(op.getOldPathMaxArrayTraversals(), 1);
}

TEST(DocumentTransformationTest, OtherRenamePathDefaults) {
    RenamePath op{"a.b.c", "x.y.z"};
    EXPECT_EQ(op.getNewPath(), "a.b.c"sv);
    EXPECT_EQ(op.getOldPath(), "x.y.z"sv);
    EXPECT_EQ(op.getNewPathMaxArrayTraversals(), 2);
    EXPECT_EQ(op.getOldPathMaxArrayTraversals(), 2);
}

class TestInterface {
public:
    void describeTransformation(DocumentOperationVisitor& visitor) const {
        visitor(ReplaceRoot{});
        visitor(PreservePath{"preserve"});
        visitor(ModifyPath{"modify", ModifiedPrefixPolicy::kNotSupported});
        visitor(RenamePath{"renameTo", "renameFrom"});
    }
};

TEST(DocumentTransformationTest, WorksWithOverloadedVisitor) {
    TestInterface test;

    bool replaced = false;
    std::string preserved;
    std::string modified;
    std::pair<std::string, std::string> renamed;
    describeTransformation(
        OverloadedVisitor{
            [&replaced](const ReplaceRoot&) { replaced = true; },
            [&preserved](const PreservePath& op) { preserved = std::string(op.getPath()); },
            [&modified](const ModifyPath& op) { modified = std::string(op.getPath()); },
            [&renamed](const RenamePath& op) {
                renamed = {std::string(op.getNewPath()), std::string(op.getOldPath())};
            },
        },
        test);

    EXPECT_TRUE(replaced);
    EXPECT_EQ(preserved, "preserve"sv);
    EXPECT_EQ(modified, "modify"sv);
    EXPECT_EQ(renamed, std::make_pair("renameTo"s, "renameFrom"s));
}

class TestVisitor : public DocumentOperationVisitor {
public:
    void operator()(const ReplaceRoot& op) override {
        ASSERT_FALSE(replacedRoot);
        replacedRoot = true;
        replacedRootIsKnownEmpty = op.isEmpty();
    }
    void operator()(const PreservePath& op) override {
        ASSERT_TRUE(replacedRoot);
        preserved.emplace_back(op.getPath());
    }
    void operator()(const ModifyPath& op) override {
        modified.emplace_back(op.getPath());
    }
    void operator()(const RenamePath& op) override {
        renamed.emplace_back(op.getNewPath(), op.getOldPath());
        maxArrayTraversals.emplace(
            op.getNewPath(),
            std::make_pair(op.getNewPathMaxArrayTraversals(), op.getOldPathMaxArrayTraversals()));
    }

    bool replacedRoot{false};
    bool replacedRootIsKnownEmpty{false};
    std::vector<std::string> preserved;
    std::vector<std::string> modified;
    std::vector<std::pair<std::string, std::string>> renamed;
    // Array traversal information from renames as (newPath, oldPath).
    mongo::StringMap<std::pair<int, int>> maxArrayTraversals;
};

GetModPathsReturn roundtrip(const GetModPathsReturn& modPaths) {
    struct RoundtripHelper {
        RoundtripHelper(const GetModPathsReturn& modPaths) : modPaths(modPaths) {}
        void describeTransformation(DocumentOperationVisitor& visitor) const {
            document_transformation::describeGetModPathsReturn(visitor, modPaths);
        }
        const GetModPathsReturn& modPaths;
    };
    return toGetModPathsReturn(RoundtripHelper(modPaths));
}

void assertRoundtripsCleanly(const GetModPathsReturn& modPaths) {
    auto converted = roundtrip(modPaths);
    EXPECT_EQ(modPaths.type, converted.type);
    EXPECT_EQ(modPaths.paths, converted.paths);
    EXPECT_EQ(modPaths.renames, converted.renames);
    EXPECT_EQ(modPaths.complexRenames, converted.complexRenames);
}

TEST(DocumentTransformationTest, FromAllPaths) {
    GetModPathsReturn modPaths{
        GetModPathsReturn::Type::kAllPaths,
        {},
        {},
        {},
    };

    TestVisitor visitor;
    describeGetModPathsReturn(visitor, modPaths);

    EXPECT_TRUE(visitor.replacedRoot);
    EXPECT_FALSE(visitor.replacedRootIsKnownEmpty);
    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, IsEmpty());
    assertRoundtripsCleanly(modPaths);
}

TEST(DocumentTransformationTest, FromNotSupported) {
    GetModPathsReturn modPaths{
        GetModPathsReturn::Type::kNotSupported,
        {},
        {},
        {},
    };

    TestVisitor visitor;
    describeGetModPathsReturn(visitor, modPaths);

    EXPECT_TRUE(visitor.replacedRoot);
    EXPECT_FALSE(visitor.replacedRootIsKnownEmpty);
    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, IsEmpty());
    EXPECT_EQ(GetModPathsReturn::Type::kAllPaths, roundtrip(modPaths).type);
}

TEST(DocumentTransformationTest, FromEmptyAllExcept) {
    GetModPathsReturn modPaths{
        GetModPathsReturn::Type::kAllExcept,
        {},
        {},
        {},
    };

    TestVisitor visitor;
    describeGetModPathsReturn(visitor, modPaths);

    EXPECT_TRUE(visitor.replacedRoot);
    EXPECT_FALSE(visitor.replacedRootIsKnownEmpty);
    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, IsEmpty());
    EXPECT_EQ(GetModPathsReturn::Type::kAllPaths, roundtrip(modPaths).type);
}

TEST(DocumentTransformationTest, FromAllExcept) {
    GetModPathsReturn modPaths{
        GetModPathsReturn::Type::kAllExcept,
        {"a"},
        {},
        {},
    };

    TestVisitor visitor;
    describeGetModPathsReturn(visitor, modPaths);

    EXPECT_TRUE(visitor.replacedRoot);
    EXPECT_FALSE(visitor.replacedRootIsKnownEmpty);
    EXPECT_THAT(visitor.preserved, UnorderedElementsAre("a"s));
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, IsEmpty());
    assertRoundtripsCleanly(modPaths);
}

TEST(DocumentTransformationTest, FromAllExceptWithSimpleRename) {
    GetModPathsReturn modPaths{
        GetModPathsReturn::Type::kAllExcept,
        {"a"},
        {{"b", "c"}},
        {},
    };

    TestVisitor visitor;
    describeGetModPathsReturn(visitor, modPaths);

    EXPECT_TRUE(visitor.replacedRoot);
    EXPECT_THAT(visitor.preserved, UnorderedElementsAre("a"s));
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, UnorderedElementsAre(Pair("b"s, "c"s)));
    assertRoundtripsCleanly(modPaths);
}


TEST(DocumentTransformationTest, FromAllExceptWithComplexRename) {
    GetModPathsReturn modPaths{
        GetModPathsReturn::Type::kAllExcept,
        {"a"},
        {},
        {{"b", "c.d"}},
    };

    TestVisitor visitor;
    describeGetModPathsReturn(visitor, modPaths);

    EXPECT_TRUE(visitor.replacedRoot);
    EXPECT_THAT(visitor.preserved, UnorderedElementsAre("a"s));
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, UnorderedElementsAre(Pair("b"s, "c.d"s)));
    assertRoundtripsCleanly(modPaths);
}

TEST(DocumentTransformationTest, FromEmptyFiniteSet) {
    GetModPathsReturn modPaths{
        GetModPathsReturn::Type::kFiniteSet,
        {},
        {},
        {},
    };

    TestVisitor visitor;
    describeGetModPathsReturn(visitor, modPaths);

    EXPECT_FALSE(visitor.replacedRoot);
    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, IsEmpty());
    assertRoundtripsCleanly(modPaths);
}

TEST(DocumentTransformationTest, FromFiniteSet) {
    GetModPathsReturn modPaths{
        GetModPathsReturn::Type::kFiniteSet,
        {"a"},
        {},
        {},
    };

    TestVisitor visitor;
    describeGetModPathsReturn(visitor, modPaths);

    EXPECT_FALSE(visitor.replacedRoot);
    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("a"s));
    EXPECT_THAT(visitor.renamed, IsEmpty());
    assertRoundtripsCleanly(modPaths);
}

TEST(DocumentTransformationTest, FromFiniteSetWithSimpleRename) {
    GetModPathsReturn modPaths{
        GetModPathsReturn::Type::kFiniteSet,
        {"a"},
        {{"b", "c"}},
        {},
    };

    TestVisitor visitor;
    describeGetModPathsReturn(visitor, modPaths);

    EXPECT_FALSE(visitor.replacedRoot);
    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("a"s));
    EXPECT_THAT(visitor.renamed, UnorderedElementsAre(Pair("b"s, "c"s)));
    assertRoundtripsCleanly(modPaths);
}

TEST(DocumentTransformationTest, FromFiniteSetWithComplexRename) {
    GetModPathsReturn modPaths{
        GetModPathsReturn::Type::kFiniteSet,
        {"a", "b"},
        {},
        {{"b", "c.d"}},
    };

    TestVisitor visitor;
    describeGetModPathsReturn(visitor, modPaths);

    EXPECT_FALSE(visitor.replacedRoot);
    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("a"s));
    EXPECT_THAT(visitor.renamed, UnorderedElementsAre(Pair("b"s, "c.d"s)));
    assertRoundtripsCleanly(modPaths);
}

TEST(DocumentTransformationTest, DescribeInclusionPathsWithoutPrefix) {
    std::vector<std::string_view> paths{"a"sv, "b.c"sv};

    TestVisitor visitor;
    visitor(ReplaceRoot{});
    document_transformation::describeProjectedPaths(
        visitor, paths.begin(), paths.end(), {}, /* isInclusion */ true);

    EXPECT_THAT(visitor.preserved, UnorderedElementsAre("a"sv, "b.c"sv));
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeInclusionPathsWithPrefix) {
    std::vector<std::string_view> paths{"a"sv, "b.c"sv};

    TestVisitor visitor;
    visitor(ReplaceRoot{});
    document_transformation::describeProjectedPaths(
        visitor, paths.begin(), paths.end(), "root", /* isInclusion */ true);

    EXPECT_THAT(visitor.preserved, UnorderedElementsAre("root.a"sv, "root.b.c"sv));
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeExclusionPathsWithoutPrefix) {
    std::vector<std::string_view> paths{"a"sv, "b.c"sv};

    TestVisitor visitor;
    document_transformation::describeProjectedPaths(
        visitor, paths.begin(), paths.end(), {}, /* isInclusion */ false);

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("a"sv, "b.c"sv));
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeExclusionPathsWithPrefix) {
    std::vector<std::string_view> paths{"a"sv, "b.c"sv};

    TestVisitor visitor;
    document_transformation::describeProjectedPaths(
        visitor, paths.begin(), paths.end(), "root", /* isInclusion */ false);

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("root.a"sv, "root.b.c"sv));
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeComputedPathsWithoutPrefix) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionConstant::create(expCtx.get(), Value(1));
    StringMap<boost::intrusive_ptr<Expression>> paths{{"a"s, expr}, {"b.c"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("a"sv, "b.c"sv));
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeComputedPathsWithPrefix) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionConstant::create(expCtx.get(), Value(1));
    StringMap<boost::intrusive_ptr<Expression>> paths{{"a"s, expr}, {"b.c"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), "root");

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("root.a"sv, "root.b.c"sv));
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeComputedPathsRenames) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionFieldPath::parse(expCtx.get(), "$x.y", expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"a"s, expr}, {"b.c"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, UnorderedElementsAre(Pair("a"s, "x.y"s), Pair("b.c"s, "x.y"s)));
}

TEST(DocumentTransformationTest, DescribeComputedPathsSimpleRenames) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionFieldPath::parse(expCtx.get(), "$x", expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{
        {"a"s, expr}, {"b.c"s, expr}, {"d.e.f"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed,
                UnorderedElementsAre(Pair("a"s, "x"s), Pair("b.c"s, "x"s), Pair("d.e.f"s, "x"s)));

    // Check that the maxArrayTraversals is consistent with getComputedPaths.

    auto renamesForA = expr->getComputedPaths("a", Variables::kRootId).renames;
    EXPECT_TRUE(renamesForA.contains("a"));
    EXPECT_EQ(visitor.maxArrayTraversals.at("a"s), std::make_pair(0, 0));

    auto renamesForBC = expr->getComputedPaths("b.c", Variables::kRootId).renames;
    EXPECT_TRUE(renamesForBC.contains("b.c"));
    EXPECT_EQ(visitor.maxArrayTraversals.at("b.c"s), std::make_pair(0, 0));

    auto renamesForDEF = expr->getComputedPaths("d.e.f", Variables::kRootId).renames;
    EXPECT_TRUE(renamesForDEF.contains("d.e.f"));
    EXPECT_EQ(visitor.maxArrayTraversals.at("d.e.f"s), std::make_pair(0, 0));
}

TEST(DocumentTransformationTest, DescribeComputedPathsOtherRenames) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionFieldPath::parse(expCtx.get(), "$x.y.z", expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{
        {"a"s, expr}, {"b.c"s, expr}, {"d.e.f"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed,
                UnorderedElementsAre(
                    Pair("a"s, "x.y.z"s), Pair("b.c"s, "x.y.z"s), Pair("d.e.f"s, "x.y.z"s)));

    // Since we have three elements x.y.z, these renames are currently not reported as renames by
    // getComputedPaths.

    auto modifiedPathsForA = expr->getComputedPaths("a", Variables::kRootId).paths;
    EXPECT_TRUE(modifiedPathsForA.contains("a"));
    EXPECT_EQ(visitor.maxArrayTraversals.at("a"s), std::make_pair(0, 2));

    auto modifiedPathsForBC = expr->getComputedPaths("b.c", Variables::kRootId).paths;
    EXPECT_TRUE(modifiedPathsForBC.contains("b.c"));
    EXPECT_EQ(visitor.maxArrayTraversals.at("b.c"s), std::make_pair(0, 2));

    auto modifiedPathsForDEF = expr->getComputedPaths("d.e.f", Variables::kRootId).paths;
    EXPECT_TRUE(modifiedPathsForDEF.contains("d.e.f"));
    EXPECT_EQ(visitor.maxArrayTraversals.at("d.e.f"s), std::make_pair(0, 2));
}

TEST(DocumentTransformationTest, DescribeComputedPathsUsingVariable) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionFieldPath::parse(expCtx.get(), "$$ROOT.x", expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"a"s, expr}, {"b.c"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, UnorderedElementsAre(Pair("a"s, "x"s), Pair("b.c"s, "x"s)));
}

TEST(DocumentTransformationTest, DescribeComputedPathsROOT) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionFieldPath::parse(expCtx.get(), "$$ROOT", expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"a"s, expr}, {"b.c"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("a"s, "b.c"s));
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeComputedPathsCURRENT) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionFieldPath::parse(expCtx.get(), "$$CURRENT", expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"a"s, expr}, {"b.c"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("a"s, "b.c"s));
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeComputedPathsOtherVariable) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionFieldPath::parse(expCtx.get(), "$$NOW", expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"a"s, expr}, {"b.c"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("a"s, "b.c"s));
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeComputedPathsObjectEmpty) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionObject::parse(expCtx.get(), fromjson("{}"), expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"_id"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeComputedPathsObjectModifications) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionObject::parse(
        expCtx.get(), fromjson("{a: 1, b: {c: 1}}"), expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"_id"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("_id.a"sv, "_id.b.c"sv));
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeComputedPathsObjectSimpleRenames) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionObject::parse(
        expCtx.get(), fromjson("{a: {b: '$x'}}"), expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"_id"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, UnorderedElementsAre(Pair("_id.a.b"s, "x"s)));
    // _id.a and _id.a.b cannot contain arrays.
    EXPECT_EQ(visitor.maxArrayTraversals.at("_id.a.b"s), std::make_pair(0, 0));

    // The maxArrayTraversals indicating no arrays should be consistent with 'renames',
    // since $x also does not require array traversal.
    auto renames = expr->getComputedPaths("_id", Variables::kRootId).renames;
    EXPECT_TRUE(renames.contains("_id.a.b"));
}

TEST(DocumentTransformationTest, DescribeComputedPathsObjectComplexRenames) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionObject::parse(
        expCtx.get(), fromjson("{a: {b: '$x.y'}}"), expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"_id"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, UnorderedElementsAre(Pair("_id.a.b"s, "x.y"s)));
    // _id.a and _id.a.b cannot contain arrays, x.y may contain 1.
    EXPECT_EQ(visitor.maxArrayTraversals.at("_id.a.b"s), std::make_pair(0, 1));

    // The maxArrayTraversals indicating no arrays and x.y on the right should be
    // consistent with complex renames normally, however ExpressionObject does not support these
    // in getComputedPaths so it reports them as modified.
    auto modifiedPaths = expr->getComputedPaths("_id", Variables::kRootId).paths;
    EXPECT_TRUE(modifiedPaths.contains("_id.a.b"));
}

TEST(DocumentTransformationTest, DescribeComputedPathsObjectOtherRenames) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionObject::parse(expCtx.get(),
                                        fromjson("{a: {b: {c: '$x.y.z'}}, c: '$x.y.z'}"),
                                        expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"_id"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.renamed,
                UnorderedElementsAre(Pair("_id.a.b.c"s, "x.y.z"s), Pair("_id.c"s, "x.y.z"s)));
    // _id.a, _id.a.b, _id.a.b.c cannot contain arrays (nested objects).
    EXPECT_EQ(visitor.maxArrayTraversals.at("_id.a.b.c"s), std::make_pair(0, 2));
    // _id.c cannot contain arrays.
    EXPECT_EQ(visitor.maxArrayTraversals.at("_id.c"s), std::make_pair(0, 2));

    // Renames with more than 1 component were not reported anywhere and were treated as modified
    // paths.
    auto modifiedPaths = expr->getComputedPaths("_id", Variables::kRootId).paths;
    EXPECT_TRUE(modifiedPaths.contains("_id.a.b.c"));
    EXPECT_TRUE(modifiedPaths.contains("_id.c"));
}

TEST(DocumentTransformationTest, DescribeComputedPathsObjectMixed) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionObject::parse(expCtx.get(),
                                        fromjson("{a: {b: '$x.y.z', c: 1}, d: {$toInt: '1'}}"),
                                        expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"_id"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("_id.a.c"sv, "_id.d"sv));
    EXPECT_THAT(visitor.renamed, UnorderedElementsAre(Pair("_id.a.b"s, "x.y.z"s)));
    // _id.a and _id.a.b cannot contain arrays.
    EXPECT_EQ(visitor.maxArrayTraversals.at("_id.a.b"s), std::make_pair(0, 2));
}

TEST(DocumentTransformationTest, DescribeComputedPathsObjectROOT) {
    auto nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest(nss));
    auto expr = ExpressionObject::parse(
        expCtx.get(), fromjson("{a: '$$ROOT', d: 1}"), expCtx->variablesParseState);
    StringMap<boost::intrusive_ptr<Expression>> paths{{"_id"s, expr}};

    TestVisitor visitor;
    document_transformation::describeComputedPaths(visitor, paths.begin(), paths.end(), {});

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("_id.a"sv, "_id.d"sv));
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

}  // namespace
}  // namespace mongo::document_transformation
