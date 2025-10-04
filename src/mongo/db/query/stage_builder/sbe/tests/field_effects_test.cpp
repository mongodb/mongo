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
#include "mongo/db/query/stage_builder/sbe/analysis.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

namespace mongo::stage_builder {
namespace {

TEST(FieldEffectsTest, BasicTest) {
    auto projEffectsA = [&] {
        bool isInclusion = true;
        std::vector<std::string> paths = {"a", "b", "d", "e", "c.x"};
        std::vector<ProjectNode> nodes;
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(SbExpr{});

        return FieldEffects(isInclusion, paths, nodes);
    }();

    auto projEffectsB = [&] {
        bool isInclusion = true;
        std::vector<std::string> paths = {"a", "d.x", "e.y", "b"};
        std::vector<ProjectNode> nodes;
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(ProjectNode::Keep{});
        nodes.emplace_back(SbExpr{});
        nodes.emplace_back(SbExpr{});

        return FieldEffects(isInclusion, paths, nodes);
    }();

    auto projEffectsC = [&] {
        bool isInclusion = false;
        std::vector<std::string> paths = {"d", "b.x.y", "c"};
        std::vector<ProjectNode> nodes;
        nodes.emplace_back(ProjectNode::Drop{});
        nodes.emplace_back(ProjectNode::Drop{});
        nodes.emplace_back(SbExpr{});

        return FieldEffects(isInclusion, paths, nodes);
    }();

    auto projEffectsD = [&] {
        bool isInclusion = false;
        std::vector<std::string> paths = {"e", "b.x.z", "c"};
        std::vector<ProjectNode> nodes;
        nodes.emplace_back(ProjectNode::Drop{});
        nodes.emplace_back(ProjectNode::Drop{});
        nodes.emplace_back(SbExpr{});

        return FieldEffects(isInclusion, paths, nodes);
    }();

    // Verify the FieldEffects objects we constructed have the Effects we expect them to have.
    ASSERT_TRUE(projEffectsA.get("a") == FieldEffect::kKeep);
    ASSERT_TRUE(projEffectsA.get("b") == FieldEffect::kKeep);
    ASSERT_TRUE(projEffectsA.get("c") == FieldEffect::kSet);
    ASSERT_TRUE(projEffectsA.get("d") == FieldEffect::kKeep);
    ASSERT_TRUE(projEffectsA.get("e") == FieldEffect::kKeep);
    ASSERT_TRUE(projEffectsA.getDefaultEffect() == FieldEffect::kDrop);

    ASSERT_TRUE(projEffectsB.get("a") == FieldEffect::kKeep);
    ASSERT_TRUE(projEffectsB.get("b") == FieldEffect::kAdd);
    ASSERT_TRUE(projEffectsB.get("c") == FieldEffect::kDrop);
    ASSERT_TRUE(projEffectsB.get("d") == FieldEffect::kModify);
    ASSERT_TRUE(projEffectsB.get("e") == FieldEffect::kSet);
    ASSERT_TRUE(projEffectsB.getDefaultEffect() == FieldEffect::kDrop);

    ASSERT_TRUE(projEffectsC.get("a") == FieldEffect::kKeep);
    ASSERT_TRUE(projEffectsC.get("b") == FieldEffect::kModify);
    ASSERT_TRUE(projEffectsC.get("c") == FieldEffect::kSet);
    ASSERT_TRUE(projEffectsC.get("d") == FieldEffect::kDrop);
    ASSERT_TRUE(projEffectsC.get("e") == FieldEffect::kKeep);
    ASSERT_TRUE(projEffectsC.getDefaultEffect() == FieldEffect::kKeep);

    ASSERT_TRUE(projEffectsD.get("a") == FieldEffect::kKeep);
    ASSERT_TRUE(projEffectsD.get("b") == FieldEffect::kModify);
    ASSERT_TRUE(projEffectsD.get("c") == FieldEffect::kSet);
    ASSERT_TRUE(projEffectsD.get("d") == FieldEffect::kKeep);
    ASSERT_TRUE(projEffectsD.get("e") == FieldEffect::kDrop);
    ASSERT_TRUE(projEffectsD.getDefaultEffect() == FieldEffect::kKeep);

    // Verify that merge() and compose() behave as expected.
    auto composedCA = FieldEffects(projEffectsC);
    composedCA.compose(projEffectsA);

    auto composedDA = FieldEffects(projEffectsD);
    composedDA.compose(projEffectsA);

    auto composedCB = FieldEffects(projEffectsC);
    composedCB.compose(projEffectsB);

    auto composedDB = FieldEffects(projEffectsD);
    composedDB.compose(projEffectsB);

    ASSERT_TRUE(composedCA.get("a") == FieldEffect::kKeep);
    ASSERT_TRUE(composedCA.get("b") == FieldEffect::kModify);
    ASSERT_TRUE(composedCA.get("c") == FieldEffect::kGeneric);
    ASSERT_TRUE(composedCA.get("d") == FieldEffect::kDrop);
    ASSERT_TRUE(composedCA.get("e") == FieldEffect::kKeep);
    ASSERT_TRUE(composedCA.getDefaultEffect() == FieldEffect::kDrop);

    ASSERT_TRUE(composedDA.get("a") == FieldEffect::kKeep);
    ASSERT_TRUE(composedDA.get("b") == FieldEffect::kModify);
    ASSERT_TRUE(composedDA.get("c") == FieldEffect::kGeneric);
    ASSERT_TRUE(composedDA.get("d") == FieldEffect::kKeep);
    ASSERT_TRUE(composedDA.get("e") == FieldEffect::kDrop);
    ASSERT_TRUE(composedDA.getDefaultEffect() == FieldEffect::kDrop);

    ASSERT_TRUE(composedCB.get("a") == FieldEffect::kKeep);
    ASSERT_TRUE(composedCB.get("b") == FieldEffect::kAdd);
    ASSERT_TRUE(composedCB.get("c") == FieldEffect::kDrop);
    ASSERT_TRUE(composedCB.get("d") == FieldEffect::kDrop);
    ASSERT_TRUE(composedCB.get("e") == FieldEffect::kSet);
    ASSERT_TRUE(composedCB.getDefaultEffect() == FieldEffect::kDrop);

    ASSERT_TRUE(composedDB.get("a") == FieldEffect::kKeep);
    ASSERT_TRUE(composedDB.get("b") == FieldEffect::kAdd);
    ASSERT_TRUE(composedDB.get("c") == FieldEffect::kDrop);
    ASSERT_TRUE(composedDB.get("d") == FieldEffect::kModify);
    ASSERT_TRUE(composedDB.get("e") == FieldEffect::kAdd);
    ASSERT_TRUE(composedDB.getDefaultEffect() == FieldEffect::kDrop);

    // Verify that making the round-trip from FieldEffects to "3-FieldSet form" and back
    // will perfectly preserve all of the Effects (and the default Effect as well).
    std::vector<FieldEffects> vec;
    vec.push_back(projEffectsA);
    vec.push_back(projEffectsB);
    vec.push_back(projEffectsC);
    vec.push_back(projEffectsD);
    vec.push_back(composedCA);
    vec.push_back(composedDA);
    vec.push_back(composedCB);
    vec.push_back(composedDB);

    for (auto&& pe : vec) {
        auto roundTrip =
            FieldEffects(pe.getAllowedFields(), pe.getChangedFields(), pe.getCreatedFieldVector());

        roundTrip.setFieldOrder(pe.getFieldList());

        ASSERT_EQ(pe.getFieldList(), roundTrip.getFieldList());

        ASSERT_EQ(pe.getDefaultEffect(), roundTrip.getDefaultEffect());

        for (auto&& field : pe.getFieldList()) {
            ASSERT_EQ(pe.get(field), roundTrip.get(field));
        }
    }
}

}  // namespace
}  // namespace mongo::stage_builder
