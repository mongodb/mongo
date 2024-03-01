/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

/**
 * This file contains tests for sbe::HashJoinStage.
 */

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/inlined_vector.h>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::sbe {

using HashJoinStageTest = PlanStageTestFixture;

TEST_F(HashJoinStageTest, HashJoinCollationTest) {
    using namespace std::literals;
    for (auto useCollator : {false, true}) {
        auto [innerTag, innerVal] = stage_builder::makeValue(BSON_ARRAY("a"
                                                                        << "b"
                                                                        << "c"));
        value::ValueGuard innerGuard{innerTag, innerVal};

        auto [outerTag, outerVal] = stage_builder::makeValue(BSON_ARRAY("a"
                                                                        << "b"
                                                                        << "A"));
        value::ValueGuard outerGuard{outerTag, outerVal};

        // After running the join we expect to get back pairs of the keys that were
        // matched up.
        std::vector<std::pair<std::string, std::string>> expectedVec;
        if (useCollator) {
            expectedVec = {{"a", "A"}, {"a", "a"}, {"b", "b"}};
        } else {
            expectedVec = {{"a", "a"}, {"b", "b"}};
        }

        auto collatorSlot = generateSlotId();

        auto makeStageFn = [this, collatorSlot, useCollator](
                               value::SlotId outerCondSlot,
                               value::SlotId innerCondSlot,
                               std::unique_ptr<PlanStage> outerStage,
                               std::unique_ptr<PlanStage> innerStage) {
            auto hashJoinStage =
                makeS<HashJoinStage>(std::move(outerStage),
                                     std::move(innerStage),
                                     makeSV(outerCondSlot),
                                     makeSV(),
                                     makeSV(innerCondSlot),
                                     makeSV(),
                                     boost::optional<value::SlotId>{useCollator, collatorSlot},
                                     nullptr /* yieldPolicy */,
                                     kEmptyPlanNodeId);

            return std::make_pair(makeSV(innerCondSlot, outerCondSlot), std::move(hashJoinStage));
        };

        auto ctx = makeCompileCtx();

        // Setup collator and insert it into the ctx.
        auto collator = std::make_unique<CollatorInterfaceMock>(
            CollatorInterfaceMock::MockType::kToLowerString);
        value::OwnedValueAccessor collatorAccessor;
        ctx->pushCorrelated(collatorSlot, &collatorAccessor);
        collatorAccessor.reset(value::TypeTags::collator,
                               value::bitcastFrom<CollatorInterface*>(collator.release()));

        // Two separate virtual scans are needed since HashJoinStage needs two child stages.
        outerGuard.reset();
        auto [outerCondSlot, outerStage] = generateVirtualScan(outerTag, outerVal);

        innerGuard.reset();
        auto [innerCondSlot, innerStage] = generateVirtualScan(innerTag, innerVal);

        // Call the `makeStage` callback to create the HashJoinStage, passing in the mock scan
        // subtrees and the subtree's output slots.
        auto [outputSlots, stage] =
            makeStageFn(outerCondSlot, innerCondSlot, std::move(outerStage), std::move(innerStage));

        // Prepare the tree and get the SlotAccessor for the output slots.
        auto resultAccessors = prepareTree(ctx.get(), stage.get(), outputSlots);

        // Get all the results produced by HashJoin.
        auto [resultsTag, resultsVal] = getAllResultsMulti(stage.get(), resultAccessors);
        value::ValueGuard resultsGuard{resultsTag, resultsVal};
        ASSERT_EQ(resultsTag, value::TypeTags::Array);
        auto resultsView = value::getArrayView(resultsVal);

        // make sure all the expected pairs occur in the result
        ASSERT_EQ(resultsView->size(), expectedVec.size());
        for (const auto& [outer, inner] : expectedVec) {
            auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(outer << inner));
            bool found = false;
            for (size_t i = 0; i < resultsView->size(); i++) {
                auto [tag, val] = resultsView->getAt(i);
                auto [cmpTag, cmpVal] = compareValue(expectedTag, expectedVal, tag, val);
                if (cmpTag == value::TypeTags::NumberInt32 &&
                    value::bitcastTo<int32_t>(cmpVal) == 0) {
                    found = true;
                    break;
                }
            }

            ASSERT_TRUE(found);

            releaseValue(expectedTag, expectedVal);
        }
    }
}

}  // namespace mongo::sbe
