/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_builder_helpers.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/field_set.h"

namespace mongo::stage_builder {
namespace {

TEST(ProjectionEffectsTest, BasicTest) {
    auto projEffectsA = [&] {
        bool isInclusion = true;
        std::vector<std::string> paths = {"a", "b", "d", "e", "c.x"};
        std::vector<ProjectNode> nodes;
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(SbExpr{});

        return ProjectionEffects(isInclusion, paths, nodes);
    }();

    auto projEffectsB = [&] {
        bool isInclusion = true;
        std::vector<std::string> paths = {"a", "d.x", "e.y", "b"};
        std::vector<ProjectNode> nodes;
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(SbExpr{});
        nodes.emplace_back(SbExpr{});

        return ProjectionEffects(isInclusion, paths, nodes);
    }();

    auto projEffectsC = [&] {
        bool isInclusion = false;
        std::vector<std::string> paths = {"d", "b.x.y", "c"};
        std::vector<ProjectNode> nodes;
        nodes.emplace_back(ProjectNode::Drop{});
        nodes.emplace_back(ProjectNode::Drop{});
        nodes.emplace_back(SbExpr{});

        return ProjectionEffects(isInclusion, paths, nodes);
    }();

    auto projEffectsD = [&] {
        bool isInclusion = false;
        std::vector<std::string> paths = {"e", "b.x.z", "c"};
        std::vector<ProjectNode> nodes;
        nodes.emplace_back(ProjectNode::Drop{});
        nodes.emplace_back(ProjectNode::Drop{});
        nodes.emplace_back(SbExpr{});

        return ProjectionEffects(isInclusion, paths, nodes);
    }();

    // Verify the ProjectionEffects objects we constructed have the Effects we expect them to have.
    ASSERT_TRUE(projEffectsA.isKeep("a"));
    ASSERT_TRUE(projEffectsA.isKeep("b"));
    ASSERT_TRUE(projEffectsA.isCreate("c"));
    ASSERT_TRUE(projEffectsA.isKeep("d"));
    ASSERT_TRUE(projEffectsA.isKeep("e"));
    ASSERT_TRUE(projEffectsA.getDefaultEffect() == ProjectionEffects::kDrop);

    ASSERT_TRUE(projEffectsB.isKeep("a"));
    ASSERT_TRUE(projEffectsB.isCreate("b"));
    ASSERT_TRUE(projEffectsB.isDrop("c"));
    ASSERT_TRUE(projEffectsB.isModify("d"));
    ASSERT_TRUE(projEffectsB.isCreate("e"));
    ASSERT_TRUE(projEffectsB.getDefaultEffect() == ProjectionEffects::kDrop);

    ASSERT_TRUE(projEffectsC.isKeep("a"));
    ASSERT_TRUE(projEffectsC.isModify("b"));
    ASSERT_TRUE(projEffectsC.isCreate("c"));
    ASSERT_TRUE(projEffectsC.isDrop("d"));
    ASSERT_TRUE(projEffectsC.isKeep("e"));
    ASSERT_TRUE(projEffectsC.getDefaultEffect() == ProjectionEffects::kKeep);

    ASSERT_TRUE(projEffectsD.isKeep("a"));
    ASSERT_TRUE(projEffectsD.isModify("b"));
    ASSERT_TRUE(projEffectsD.isCreate("c"));
    ASSERT_TRUE(projEffectsD.isKeep("d"));
    ASSERT_TRUE(projEffectsD.isDrop("e"));
    ASSERT_TRUE(projEffectsD.getDefaultEffect() == ProjectionEffects::kKeep);

    // Verify that merge() and compose() behave as expected.
    auto composedAC = ProjectionEffects(projEffectsA).compose(projEffectsC);
    auto composedAD = ProjectionEffects(projEffectsA).compose(projEffectsD);
    auto composedBC = ProjectionEffects(projEffectsB).compose(projEffectsC);
    auto composedBD = ProjectionEffects(projEffectsB).compose(projEffectsD);
    auto mergedACAD = ProjectionEffects(composedAC).merge(composedAD);
    auto mergedBCBD = ProjectionEffects(composedBC).merge(composedBD);

    ASSERT_TRUE(composedAC.isKeep("a"));
    ASSERT_TRUE(composedAC.isModify("b"));
    ASSERT_TRUE(composedAC.isCreate("c"));
    ASSERT_TRUE(composedAC.isDrop("d"));
    ASSERT_TRUE(composedAC.isKeep("e"));
    ASSERT_TRUE(composedAC.getDefaultEffect() == ProjectionEffects::kDrop);

    ASSERT_TRUE(composedAD.isKeep("a"));
    ASSERT_TRUE(composedAD.isModify("b"));
    ASSERT_TRUE(composedAD.isCreate("c"));
    ASSERT_TRUE(composedAD.isKeep("d"));
    ASSERT_TRUE(composedAD.isDrop("e"));
    ASSERT_TRUE(composedAD.getDefaultEffect() == ProjectionEffects::kDrop);

    ASSERT_TRUE(mergedACAD.isKeep("a"));
    ASSERT_TRUE(mergedACAD.isModify("b"));
    ASSERT_TRUE(mergedACAD.isCreate("c"));
    ASSERT_TRUE(mergedACAD.isModify("d"));
    ASSERT_TRUE(mergedACAD.isModify("e"));
    ASSERT_TRUE(mergedACAD.getDefaultEffect() == ProjectionEffects::kDrop);

    ASSERT_TRUE(composedBC.isKeep("a"));
    ASSERT_TRUE(composedBC.isCreate("b"));
    ASSERT_TRUE(composedBC.isDrop("c"));
    ASSERT_TRUE(composedBC.isDrop("d"));
    ASSERT_TRUE(composedBC.isCreate("e"));
    ASSERT_TRUE(composedBC.getDefaultEffect() == ProjectionEffects::kDrop);

    ASSERT_TRUE(composedBD.isKeep("a"));
    ASSERT_TRUE(composedBD.isCreate("b"));
    ASSERT_TRUE(composedBD.isDrop("c"));
    ASSERT_TRUE(composedBD.isModify("d"));
    ASSERT_TRUE(composedBD.isCreate("e"));
    ASSERT_TRUE(composedBD.getDefaultEffect() == ProjectionEffects::kDrop);

    ASSERT_TRUE(mergedBCBD.isKeep("a"));
    ASSERT_TRUE(mergedBCBD.isCreate("b"));
    ASSERT_TRUE(mergedBCBD.isDrop("c"));
    ASSERT_TRUE(mergedBCBD.isModify("d"));
    ASSERT_TRUE(mergedBCBD.isCreate("e"));
    ASSERT_TRUE(mergedBCBD.getDefaultEffect() == ProjectionEffects::kDrop);

    // Verify that '(A+B)*(C+D) == (A*C)+(B*D)' is true (where '+' is merge and '*' is compose).
    auto mergedACBD = ProjectionEffects(composedAC).merge(composedBD);
    auto composedABCD = ProjectionEffects(projEffectsA)
                            .merge(projEffectsB)
                            .compose(ProjectionEffects(projEffectsC).merge(projEffectsD));

    ASSERT_EQ(composedABCD.get("a"), mergedACBD.get("a"));
    ASSERT_EQ(composedABCD.get("b"), mergedACBD.get("b"));
    ASSERT_EQ(composedABCD.get("c"), mergedACBD.get("c"));
    ASSERT_EQ(composedABCD.get("d"), mergedACBD.get("d"));
    ASSERT_EQ(composedABCD.get("e"), mergedACBD.get("e"));
    ASSERT_EQ(composedABCD.getDefaultEffect(), mergedACBD.getDefaultEffect());

    // Verify that making the round-trip from ProjectionEffects to "3-FieldSet form" and back
    // will perfectly preserve all of the Effects (and the default Effect as well).
    std::vector<ProjectionEffects> vec;
    vec.push_back(projEffectsA);
    vec.push_back(projEffectsB);
    vec.push_back(projEffectsC);
    vec.push_back(projEffectsD);
    vec.push_back(composedAC);
    vec.push_back(composedAD);
    vec.push_back(composedBC);
    vec.push_back(composedBD);
    vec.push_back(composedABCD);
    vec.push_back(mergedACAD);
    vec.push_back(mergedBCBD);
    vec.push_back(mergedACBD);

    for (auto&& pe : vec) {
        auto roundTrip = ProjectionEffects(pe.getAllowedFieldSet(),
                                           pe.getModifiedOrCreatedFieldSet(),
                                           pe.getCreatedFieldSet(),
                                           pe.getFieldList());

        ASSERT_EQ(pe.getFieldList(), roundTrip.getFieldList());

        ASSERT_EQ(pe.getDefaultEffect(), roundTrip.getDefaultEffect());

        for (auto&& field : pe.getFieldList()) {
            ASSERT_EQ(pe.get(field), roundTrip.get(field));
        }
    }
}

}  // namespace
}  // namespace mongo::stage_builder
