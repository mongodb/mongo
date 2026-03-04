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

}  // namespace
}  // namespace mongo::document_transformation
