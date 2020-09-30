/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/parser/parser.h"
#include "mongo/db/exec/sbe/stages/branch.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/exchange.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/ix_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/spool.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/stages/unwind.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

/**
 * SIMPLE_PROJ and PFO stages are transformed into multiple stages by the parser. They do
 * not have a direct PlanStage analogue and are not included in these tests.
 *
 * TODO(SERVER-50885): Several stages were omitted from the tests because of inconsistencies between
 * 'debugPrint()' result and parser rules:
 * - IXSCAN, IXSEEK - slots are assigned to different values after serialization/deserialization
 *   loop
 * - PROJECT - serialization works correct, but the order of slot assignments is not preserved which
 *   prevents us from comparing debug outputs as strings
 * - MKOBJ - parser does not recognize 'forceNewObject' and 'returnOldObject' flags from debug print
 * - GROUP - slots are assigned to different values after serialization/deserialization loop
 * - LIMIT, SKIP - parser does not recognize 'limitskip' keyword produced by 'LimitSkipStage'
 * - TRAVERSE - parser does not recognize correlated slots from debug print
 * - SORT - parser does not recognize 'limit' field from debug print
 * - UNION - debug print does not output square braces around union branches list
 * - SCAN, SEEK - parser expects forward flag which is not included in the debug print
 * - HJOIN - inner and outer projects are swapped after serialization/deserialization loop
 */
class SBEParserTest : public unittest::Test {
protected:
    SBEParserTest() : planNodeId(12345) {
        stages = makeVector(
            // PSCAN
            sbe::makeS<sbe::ParallelScanStage>(NamespaceString{"testDb", "testCollection"},
                                               sbe::value::SlotId{1},
                                               sbe::value::SlotId{2},
                                               std::vector<std::string>{"a", "b"},
                                               sbe::makeSV(1, 2),
                                               nullptr,
                                               planNodeId),
            // FILTER
            sbe::makeS<sbe::FilterStage<false>>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                           sbe::value::bitcastFrom<bool>(true)),
                planNodeId),
            // CFILTER
            sbe::makeS<sbe::FilterStage<true>>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                           sbe::value::bitcastFrom<bool>(true)),
                planNodeId),
            // NLJOIN
            sbe::makeS<sbe::LoopJoinStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeSV(1, 2),
                sbe::makeSV(3, 4),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                           sbe::value::bitcastFrom<bool>(true)),
                planNodeId),
            // COSCAN
            sbe::makeS<sbe::CoScanStage>(planNodeId),
            // EXCHANGE
            sbe::makeS<sbe::ExchangeConsumer>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                              2,
                                              sbe::makeSV(1, 2),
                                              sbe::ExchangePolicy::roundrobin,
                                              nullptr,
                                              nullptr,
                                              planNodeId),
            // UNWIND
            sbe::makeS<sbe::UnwindStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                         sbe::value::SlotId{1},
                                         sbe::value::SlotId{2},
                                         sbe::value::SlotId{3},
                                         true,
                                         planNodeId),
            // BRANCH
            sbe::makeS<sbe::BranchStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                           sbe::value::bitcastFrom<bool>(true)),
                sbe::makeSV(1, 2),
                sbe::makeSV(3, 4),
                sbe::makeSV(5, 6),
                planNodeId),
            // ESPOOL
            sbe::makeS<sbe::SpoolEagerProducerStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                                     sbe::SpoolId{1},
                                                     sbe::makeSV(1, 2),
                                                     planNodeId),
            // LSPOOL
            sbe::makeS<sbe::SpoolLazyProducerStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::SpoolId{1},
                sbe::makeSV(1, 2),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Boolean,
                                           sbe::value::bitcastFrom<bool>(true)),
                planNodeId),
            // CSPOOL
            sbe::makeS<sbe::SpoolConsumerStage<false>>(
                sbe::SpoolId{1}, sbe::makeSV(1, 2), planNodeId),
            // SSPOOL
            sbe::makeS<sbe::SpoolConsumerStage<true>>(
                sbe::SpoolId{1}, sbe::makeSV(1, 2), planNodeId),

            // UNIQUE
            sbe::makeS<sbe::UniqueStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId), sbe::makeSV(1, 2), planNodeId),

            // UNION
            sbe::makeS<sbe::UnionStage>(
                ([this]() {
                    std::vector<std::unique_ptr<sbe::PlanStage>> ret;
                    ret.push_back(sbe::makeS<sbe::CoScanStage>(planNodeId));
                    ret.push_back(sbe::makeS<sbe::CoScanStage>(planNodeId));
                    return ret;
                }()),
                std::vector<sbe::value::SlotVector>{sbe::makeSV(1, 2), sbe::makeSV(3, 4)},
                sbe::makeSV(5, 6),
                planNodeId),

            // SORTED_MERGE
            sbe::makeS<sbe::SortedMergeStage>(
                ([this]() {
                    std::vector<std::unique_ptr<sbe::PlanStage>> ret;
                    ret.push_back(sbe::makeS<sbe::CoScanStage>(planNodeId));
                    ret.push_back(sbe::makeS<sbe::CoScanStage>(planNodeId));
                    return ret;
                }()),
                std::vector<sbe::value::SlotVector>{sbe::makeSV(1, 2), sbe::makeSV(3, 4)},
                std::vector<sbe::value::SortDirection>{sbe::value::SortDirection::Ascending,
                                                       sbe::value::SortDirection::Ascending},
                std::vector<sbe::value::SlotVector>{sbe::makeSV(1, 2), sbe::makeSV(3, 4)},
                sbe::makeSV(5, 6),
                planNodeId)

        );
    }

    PlanNodeId planNodeId;
    std::vector<std::unique_ptr<sbe::PlanStage>> stages;
};

TEST_F(SBEParserTest, TestIdenticalDebugOutputAfterParse) {
    sbe::DebugPrinter printer;
    sbe::Parser parser;

    for (const auto& stage : stages) {
        const auto stageText = printer.print(stage.get());
        const auto parsedStage = parser.parse(nullptr, "testDb", stageText);
        const auto stageTextAfterParse = printer.print(parsedStage.get());
        ASSERT_EQ(stageText, stageTextAfterParse);
    }
}

TEST_F(SBEParserTest, TestPlanNodeIdIsParsed) {
    sbe::DebugPrinter printer;
    sbe::Parser parser;

    for (const auto& stage : stages) {
        const auto stageText = printer.print(stage.get());
        const auto parsedStage = parser.parse(nullptr, "testDb", stageText);
        ASSERT_EQ(parsedStage->getCommonStats()->nodeId, planNodeId);
    }
}

}  // namespace
}  // namespace mongo
