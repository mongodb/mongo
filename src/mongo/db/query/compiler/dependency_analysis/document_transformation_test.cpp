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
    }

    bool replacedRoot{false};
    std::vector<std::string> preserved;
    std::vector<std::string> modified;
    std::vector<std::pair<std::string, std::string>> renamed;
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

}  // namespace
}  // namespace mongo::document_transformation
