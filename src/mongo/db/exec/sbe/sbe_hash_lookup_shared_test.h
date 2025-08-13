/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

/**
 * This file contains tests for sbe::LoopJoinStage.
 */

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/exec/sbe/util/stage_results_printer.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sbe {

class HashLookupSharedTest : public PlanStageTestFixture {
public:
    struct RunVariationParams {
        unittest::GoldenTestContext& gctx;
        std::string name;
        BSONArray outer;
        BSONArray inner;
        bool outerKeyOnly = true;
        CollatorInterface* collator = nullptr;
        bool expectMemUse = true;
    };

    virtual void runVariation(const RunVariationParams& params) = 0;

    void cloneAndEvalStage(std::ostream& stream,
                           const StageResultsPrinters::SlotNames& slotNames,
                           PlanStage* stage) {
        auto ctx = makeCompileCtx();
        auto clone = stage->clone();
        prepareAndEvalStage(ctx.get(), stream, slotNames, clone.get());
    }

    void prepareAndEvalStage(CompileCtx* ctx,
                             std::ostream& stream,
                             const StageResultsPrinters::SlotNames& slotNames,
                             PlanStage* stage) {
        prepareTree(ctx, stage);
        StageResultsPrinters::make(stream, printOptions).printStageResults(ctx, slotNames, stage);
        stage->close();
    }

    /**
     * Evaluates the stage with reopens, but only writes output once to the output stream.
     */
    void prepareAndEvalStageWithReopen(CompileCtx* ctx,
                                       std::ostream& stream,
                                       const StageResultsPrinters::SlotNames& slotNames,
                                       PlanStage* stage,
                                       bool expectMemUse = true);

public:
    PrintOptions printOptions =
        PrintOptions().arrayObjectOrNestingMaxDepth(SIZE_MAX).useTagForAmbiguousValues(true);

protected:
    static void printHashLookupStats(std::ostream& stream, const PlanStage* stage) {
        auto stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
        printHashLookupStats(stream, stats);
    }
    static void printHashLookupStats(std::ostream& stream, const HashLookupStats* stats) {
        stream << "dsk:" << stats->usedDisk << std::endl;
        stream << "htRecs:" << stats->spillingHtStats.getSpilledRecords() << std::endl;
        stream << "htIndices:" << stats->spillingHtStats.getSpilledBytes() << std::endl;
        stream << "buffRecs:" << stats->spillingBuffStats.getSpilledRecords() << std::endl;
        stream << "buffBytes:" << stats->spillingBuffStats.getSpilledBytes() << std::endl;
    }
};
}  // namespace mongo::sbe
