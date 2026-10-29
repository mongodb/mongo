// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * This file contains tests for sbe::LoopJoinStage.
 */

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
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <ostream>
#include <string>

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
                                       bool expectMemUse = true,
                                       bool getStats = true,
                                       bool executeWithSpill = true);

public:
    PrintOptions printOptions =
        PrintOptions().arrayObjectOrNestingMaxDepth(SIZE_MAX).useTagForAmbiguousValues(true);

protected:
    static void printHashLookupStats(std::ostream& stream, const PlanStage* stage) {
        auto stats = static_cast<const HashLookupStats*>(stage->getSpecificStats());
        printHashLookupStats(stream, stats);
    }
    static void printHashLookupStats(std::ostream& stream, const HashLookupStats* stats) {
        stream << "dsk:" << stats->usedDisk << '\n';
        stream << "htRecs:" << stats->spillingHtStats.getSpilledRecords() << '\n';
        stream << "htIndices:" << stats->spillingHtStats.getSpilledBytes() << '\n';
        stream << "buffRecs:" << stats->spillingBuffStats.getSpilledRecords() << '\n';
        stream << "buffBytes:" << stats->spillingBuffStats.getSpilledBytes() << '\n';
    }
};
}  // namespace mongo::sbe
