/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/dependency_analysis/document_transformation.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/document_transformation_helpers.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::document_transformation {
namespace {

using namespace mongo::unittest::match;
using namespace std::string_literals;

TEST(DocumentTransformationTest, ModifyPathDefaults) {
    ModifyPath op{"a.b.c"};
    EXPECT_EQ(op.getPath(), "a.b.c"_sd);
    EXPECT_TRUE(op.isComputed());
    EXPECT_FALSE(op.isRemoved());
    EXPECT_EQ(op.getExpression(), nullptr);
}

TEST(DocumentTransformationTest, SimpleRenamePathDefaults) {
    RenamePath op{"a", "x"};
    EXPECT_EQ(op.getNewPath(), "a"_sd);
    EXPECT_EQ(op.getOldPath(), "x"_sd);
    EXPECT_EQ(op.getNewPathMaxArrayTraversals(), 0);
    EXPECT_EQ(op.getOldPathMaxArrayTraversals(), 0);
}

TEST(DocumentTransformationTest, ComplexRenamePathDefaults) {
    RenamePath op{"a", "x.y"};
    EXPECT_EQ(op.getNewPath(), "a"_sd);
    EXPECT_EQ(op.getOldPath(), "x.y"_sd);
    EXPECT_EQ(op.getNewPathMaxArrayTraversals(), 0);
    EXPECT_EQ(op.getOldPathMaxArrayTraversals(), 1);
}

TEST(DocumentTransformationTest, OtherRenamePathDefaults) {
    RenamePath op{"a.b.c", "x.y.z"};
    EXPECT_EQ(op.getNewPath(), "a.b.c"_sd);
    EXPECT_EQ(op.getOldPath(), "x.y.z"_sd);
    EXPECT_EQ(op.getNewPathMaxArrayTraversals(), 2);
    EXPECT_EQ(op.getOldPathMaxArrayTraversals(), 2);
}

class TestInterface {
public:
    void describeTransformation(DocumentOperationVisitor& visitor) const {
        visitor(ReplaceRoot{});
        visitor(PreservePath{"preserve"});
        visitor(ModifyPath{"modify"});
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
    EXPECT_EQ(preserved, "preserve"_sd);
    EXPECT_EQ(modified, "modify"_sd);
    EXPECT_EQ(renamed, std::make_pair("renameTo"s, "renameFrom"s));
}

class TestVisitor : public DocumentOperationVisitor {
public:
    void operator()(const ReplaceRoot& op) override {
        ASSERT_FALSE(replacedRoot);
        replacedRoot = true;
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
    std::vector<StringData> paths{"a"_sd, "b.c"_sd};

    TestVisitor visitor;
    visitor(ReplaceRoot{});
    document_transformation::describeProjectedPaths(
        visitor, paths.begin(), paths.end(), {}, /* isInclusion */ true);

    EXPECT_THAT(visitor.preserved, UnorderedElementsAre("a"_sd, "b.c"_sd));
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeInclusionPathsWithPrefix) {
    std::vector<StringData> paths{"a"_sd, "b.c"_sd};

    TestVisitor visitor;
    visitor(ReplaceRoot{});
    document_transformation::describeProjectedPaths(
        visitor, paths.begin(), paths.end(), "root", /* isInclusion */ true);

    EXPECT_THAT(visitor.preserved, UnorderedElementsAre("root.a"_sd, "root.b.c"_sd));
    EXPECT_THAT(visitor.modified, IsEmpty());
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeExclusionPathsWithoutPrefix) {
    std::vector<StringData> paths{"a"_sd, "b.c"_sd};

    TestVisitor visitor;
    document_transformation::describeProjectedPaths(
        visitor, paths.begin(), paths.end(), {}, /* isInclusion */ false);

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("a"_sd, "b.c"_sd));
    EXPECT_THAT(visitor.renamed, IsEmpty());
}

TEST(DocumentTransformationTest, DescribeExclusionPathsWithPrefix) {
    std::vector<StringData> paths{"a"_sd, "b.c"_sd};

    TestVisitor visitor;
    document_transformation::describeProjectedPaths(
        visitor, paths.begin(), paths.end(), "root", /* isInclusion */ false);

    EXPECT_THAT(visitor.preserved, IsEmpty());
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("root.a"_sd, "root.b.c"_sd));
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
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("a"_sd, "b.c"_sd));
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
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("root.a"_sd, "root.b.c"_sd));
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
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("_id.a"_sd, "_id.b.c"_sd));
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
    EXPECT_THAT(visitor.modified, UnorderedElementsAre("_id.a.c"_sd, "_id.d"_sd));
    EXPECT_THAT(visitor.renamed, UnorderedElementsAre(Pair("_id.a.b"s, "x.y.z"s)));
    // _id.a and _id.a.b cannot contain arrays.
    EXPECT_EQ(visitor.maxArrayTraversals.at("_id.a.b"s), std::make_pair(0, 2));
}

}  // namespace
}  // namespace mongo::document_transformation
