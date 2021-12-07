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
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/unittest/unittest.h"

#include <regex>

namespace mongo {
namespace {
/**
 * Normalizes an SBE plan string by iterating and deterministically replacing every occurance
 * of every unique slot ID with a monotonically increasing slot counter. The SBE plan string
 * remains logically unchanged, but allows us to compare SBE plans string-to-string in test.
 */
std::string normalizeSbePlanString(const std::string& sbePlanString) {
    str::stream normalizedPlanString;

    std::map<std::string, size_t> slotsMap;
    sbe::value::SlotId slotCounter = 1;

    const std::regex slotMatchRegex("s[0-9]+");

    auto remapSlotsCallback = [&](const std::string& token) {
        std::istringstream iss(token);
        std::string n;
        if (iss >> n) {
            if (std::regex_match(token, slotMatchRegex)) {
                // If already saw this slot, replace with the one that was remapped, otherwise
                // record this new encountered slot in the map and update the counter.
                if (slotsMap.find(token) != slotsMap.end()) {
                    normalizedPlanString << "s" << std::to_string(slotsMap[token]);
                } else {
                    slotsMap[token] = slotCounter;
                    normalizedPlanString << "s" << std::to_string(slotCounter++);
                }
            } else {
                normalizedPlanString << token;
            }
        } else {
            normalizedPlanString << token;
        }
    };

    std::sregex_token_iterator begin(
        sbePlanString.begin(), sbePlanString.end(), slotMatchRegex, {-1, 0}),
        end;
    std::for_each(begin, end, remapSlotsCallback);
    return normalizedPlanString;
}

/**
 * SIMPLE_PROJ and PFO stages are transformed into multiple stages by the parser. They do
 * not have a direct PlanStage analogue and are not included in these tests.
 */
class SBEParserTest : public unittest::Test {
protected:
    SBEParserTest() : planNodeId(12345) {
        auto fakeUuid = unittest::assertGet(UUID::parse("00000000-0000-0000-0000-000000000000"));
        stages = sbe::makeSs(
            // IXSCAN with 'recordSlot' and 'recordIdSlot' slots only.
            sbe::makeS<sbe::IndexScanStage>(fakeUuid,
                                            "_id",
                                            true,
                                            sbe::value::SlotId{3},
                                            sbe::value::SlotId{4},
                                            boost::none,
                                            sbe::IndexKeysInclusionSet{},
                                            sbe::makeSV(),
                                            boost::none,
                                            boost::none,
                                            nullptr,
                                            planNodeId),
            // IXSCAN with 'seekKeySlotLow' / 'seekKeySlotHigh' present.
            sbe::makeS<sbe::IndexScanStage>(fakeUuid,
                                            "_id",
                                            true,
                                            sbe::value::SlotId{3},
                                            sbe::value::SlotId{4},
                                            boost::none,
                                            sbe::IndexKeysInclusionSet{},
                                            sbe::makeSV(),
                                            sbe::value::SlotId{1},
                                            sbe::value::SlotId{2},
                                            nullptr,
                                            planNodeId),
            // IXSCAN with 'recordIdSlot' missing and 'seekKeySlotLow' present.
            sbe::makeS<sbe::IndexScanStage>(fakeUuid,
                                            "_id",
                                            true,
                                            sbe::value::SlotId{1},
                                            boost::none,
                                            boost::none,
                                            sbe::IndexKeysInclusionSet{},
                                            sbe::makeSV(),
                                            sbe::value::SlotId{2},
                                            boost::none,
                                            nullptr,
                                            planNodeId),
            // IXSCAN with all slots except seek keys present.
            sbe::makeS<sbe::IndexScanStage>(fakeUuid,
                                            "_id",
                                            true,
                                            sbe::value::SlotId{1},
                                            sbe::value::SlotId{2},
                                            sbe::value::SlotId{3},
                                            sbe::IndexKeysInclusionSet{},
                                            sbe::makeSV(),
                                            boost::none,
                                            boost::none,
                                            nullptr,
                                            planNodeId),
            // IXSCAN with all slots present.
            sbe::makeS<sbe::IndexScanStage>(fakeUuid,
                                            "_id",
                                            true,
                                            sbe::value::SlotId{1},
                                            sbe::value::SlotId{2},
                                            sbe::value::SlotId{3},
                                            sbe::IndexKeysInclusionSet{},
                                            sbe::makeSV(),
                                            sbe::value::SlotId{4},
                                            sbe::value::SlotId{5},
                                            nullptr,
                                            planNodeId),
            // SCAN with 'recordSlot' and 'recordIdSlot' slots only.
            sbe::makeS<sbe::ScanStage>(fakeUuid,
                                       sbe::value::SlotId{1},
                                       sbe::value::SlotId{2},
                                       boost::none,
                                       boost::none,
                                       boost::none,
                                       boost::none,
                                       boost::none,
                                       std::vector<std::string>{},
                                       sbe::makeSV(),
                                       boost::none,
                                       true /* forward */,
                                       nullptr,
                                       planNodeId,
                                       sbe::ScanCallbacks{}),
            // SCAN with 'recordSlot', 'recordIdSlot' and 'seekKeySlot' slots.
            sbe::makeS<sbe::ScanStage>(fakeUuid,
                                       sbe::value::SlotId{1},
                                       sbe::value::SlotId{2},
                                       boost::none,
                                       boost::none,
                                       boost::none,
                                       boost::none,
                                       boost::none,
                                       std::vector<std::string>{},
                                       sbe::makeSV(),
                                       sbe::value::SlotId{3},
                                       true /* forward */,
                                       nullptr,
                                       planNodeId,
                                       sbe::ScanCallbacks{}),
            // SCAN with all slots present.
            sbe::makeS<sbe::ScanStage>(
                fakeUuid,
                sbe::value::SlotId{1},
                sbe::value::SlotId{2},
                sbe::value::SlotId{3},
                sbe::value::SlotId{4},
                sbe::value::SlotId{5},
                sbe::value::SlotId{6},
                sbe::value::SlotId{7},
                std::vector<std::string>{repl::OpTime::kTimestampFieldName.toString()},
                sbe::makeSV(sbe::value::SlotId{7}),
                sbe::value::SlotId{8},
                true /* forward */,
                nullptr,
                planNodeId,
                sbe::ScanCallbacks{}),
            // PSCAN with both 'recordSlot' and 'recordIdSlot' slots present.
            sbe::makeS<sbe::ParallelScanStage>(fakeUuid,
                                               sbe::value::SlotId{1},
                                               sbe::value::SlotId{2},
                                               boost::none,
                                               boost::none,
                                               boost::none,
                                               boost::none,
                                               std::vector<std::string>{"a", "b"},
                                               sbe::makeSV(1, 2),
                                               nullptr,
                                               planNodeId,
                                               sbe::ScanCallbacks{}),
            // PSCAN with 'recordSlot' missing.
            sbe::makeS<sbe::ParallelScanStage>(fakeUuid,
                                               sbe::value::SlotId{1},
                                               boost::none,
                                               boost::none,
                                               boost::none,
                                               boost::none,
                                               boost::none,
                                               std::vector<std::string>{"a", "b"},
                                               sbe::makeSV(1, 2),
                                               nullptr,
                                               planNodeId,
                                               sbe::ScanCallbacks{}),
            // PSCAN with all slots.
            sbe::makeS<sbe::ParallelScanStage>(fakeUuid,
                                               sbe::value::SlotId{1},
                                               sbe::value::SlotId{2},
                                               sbe::value::SlotId{3},
                                               sbe::value::SlotId{4},
                                               sbe::value::SlotId{5},
                                               sbe::value::SlotId{6},
                                               std::vector<std::string>{"a", "b"},
                                               sbe::makeSV(1, 2),
                                               nullptr,
                                               planNodeId,
                                               sbe::ScanCallbacks{}),
            // COSCAN
            sbe::makeS<sbe::CoScanStage>(planNodeId),
            // PROJECT
            sbe::makeProjectStage(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                planNodeId,
                sbe::value::SlotId{1},
                stage_builder::makeConstant(sbe::value::TypeTags::NumberInt32, 123),
                sbe::value::SlotId{2},
                stage_builder::makeConstant(sbe::value::TypeTags::NumberInt32, 456)),
            // TRAVERSE with only 'from' and 'in' child stages present
            sbe::makeS<sbe::TraverseStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                           sbe::makeS<sbe::CoScanStage>(planNodeId),
                                           sbe::value::SlotId{1},
                                           sbe::value::SlotId{2},
                                           sbe::value::SlotId{3},
                                           sbe::makeSV(),
                                           nullptr,
                                           nullptr,
                                           planNodeId,
                                           1 /* nestedArraysDepth */
                                           ),
            // TRAVERSE with 'outerCorrelated' slot vector present.
            sbe::makeS<sbe::TraverseStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                           sbe::makeS<sbe::CoScanStage>(planNodeId),
                                           sbe::value::SlotId{1},
                                           sbe::value::SlotId{2},
                                           sbe::value::SlotId{3},
                                           sbe::makeSV(4, 5, 6),
                                           nullptr,
                                           nullptr,
                                           planNodeId,
                                           1 /* nestedArraysDepth */
                                           ),
            // TRAVERSE with both 'foldExpr' and 'finalExpr' present.
            sbe::makeS<sbe::TraverseStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::value::SlotId{1},
                sbe::value::SlotId{2},
                sbe::value::SlotId{3},
                sbe::makeSV(4, 5, 6),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32, 123),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32, 456),
                planNodeId,
                1 /* nestedArraysDepth */
                ),
            // TRAVERSE with 'foldExpr' present but 'finalExpr' missing.
            sbe::makeS<sbe::TraverseStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::value::SlotId{1},
                sbe::value::SlotId{2},
                sbe::value::SlotId{3},
                sbe::makeSV(4, 5, 6),
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32, 123),
                nullptr,
                planNodeId,
                1 /* nestedArraysDepth */
                ),
            // TRAVERSE with 'finalExpr' present but 'foldExpr' missing.
            sbe::makeS<sbe::TraverseStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::value::SlotId{1},
                sbe::value::SlotId{2},
                sbe::value::SlotId{3},
                sbe::makeSV(4, 5, 6),
                nullptr,
                sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::NumberInt32, 123),
                planNodeId,
                1 /* nestedArraysDepth */
                ),
            // MAKEOBJ
            sbe::makeS<sbe::MakeObjStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                          sbe::value::SlotId{1},
                                          sbe::value::SlotId{2},
                                          sbe::MakeObjFieldBehavior::keep,
                                          std::vector<std::string>{"a", "b"},
                                          std::vector<std::string>{"c", "d"},
                                          sbe::makeSV(3, 4),
                                          false,
                                          false,
                                          planNodeId),
            // GROUP
            sbe::makeS<sbe::HashAggStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeSV(),
                sbe::makeEM(sbe::value::SlotId{2},
                            stage_builder::makeFunction(
                                "min", sbe::makeE<sbe::EVariable>(sbe::value::SlotId{1})),
                            sbe::value::SlotId{3},
                            stage_builder::makeFunction(
                                "max", sbe::makeE<sbe::EVariable>(sbe::value::SlotId{1}))),
                sbe::makeSV(),
                true,
                boost::none, /* optional collator slot */
                true,        /* allowDiskUse */
                planNodeId),
            // GROUP with a collator slot.
            sbe::makeS<sbe::HashAggStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeSV(),
                sbe::makeEM(sbe::value::SlotId{2},
                            stage_builder::makeFunction(
                                "min", sbe::makeE<sbe::EVariable>(sbe::value::SlotId{1})),
                            sbe::value::SlotId{3},
                            stage_builder::makeFunction(
                                "max", sbe::makeE<sbe::EVariable>(sbe::value::SlotId{1}))),
                sbe::makeSV(),
                true,
                sbe::value::SlotId{4}, /* optional collator slot */
                true,                  /* allowDiskUse */
                planNodeId),
            // LIMIT
            sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId), 100, boost::none, planNodeId),
            // SKIP
            sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId), boost::none, 100, planNodeId),
            // LIMIT SKIP
            sbe::makeS<sbe::LimitSkipStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId), 100, 200, planNodeId),
            // SORT
            sbe::makeS<sbe::SortStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeSV(1),
                std::vector<sbe::value::SortDirection>{sbe::value::SortDirection::Ascending},
                sbe::makeSV(2),
                std::numeric_limits<size_t>::max(),
                std::numeric_limits<size_t>::max(),
                true,
                planNodeId),
            // SORT with sort direction 'Descending'.
            sbe::makeS<sbe::SortStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeSV(1),
                std::vector<sbe::value::SortDirection>{sbe::value::SortDirection::Descending},
                sbe::makeSV(2),
                std::numeric_limits<size_t>::max(),
                std::numeric_limits<size_t>::max(),
                true,
                planNodeId),
            // SORT with 'limit' other than size_t max.
            sbe::makeS<sbe::SortStage>(
                sbe::makeS<sbe::CoScanStage>(planNodeId),
                sbe::makeSV(1),
                std::vector<sbe::value::SortDirection>{sbe::value::SortDirection::Ascending},
                sbe::makeSV(2),
                100 /* limit other than std::numeric_limits<size_t>::max() */,
                std::numeric_limits<size_t>::max(),
                true,
                planNodeId),
            // HJOIN
            sbe::makeS<sbe::HashJoinStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                           sbe::makeS<sbe::CoScanStage>(planNodeId),
                                           sbe::makeSV(1, 2) /* outer conditions */,
                                           sbe::makeSV(3, 4) /* outer projections */,
                                           sbe::makeSV(1, 2) /* inner conditions */,
                                           sbe::makeSV(5, 6) /* inner projections */,
                                           boost::none, /* optional collator slot */
                                           planNodeId),
            // HJOIN with a collator slot.
            sbe::makeS<sbe::HashJoinStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                           sbe::makeS<sbe::CoScanStage>(planNodeId),
                                           sbe::makeSV(1, 2) /* outer conditions */,
                                           sbe::makeSV(3, 4) /* outer projections */,
                                           sbe::makeSV(1, 2) /* inner conditions */,
                                           sbe::makeSV(5, 6) /* inner projections */,
                                           sbe::value::SlotId{7}, /* optional collator slot */
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
            sbe::makeS<sbe::MakeObjStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                          sbe::value::SlotId{1},
                                          sbe::value::SlotId{2},
                                          sbe::MakeObjStage::FieldBehavior::drop,
                                          std::vector<std::string>{"restricted", "fields"},
                                          std::vector<std::string>{"projected", "fields"},
                                          sbe::makeSV(3, 4),
                                          false,
                                          false,
                                          planNodeId),
            sbe::makeS<sbe::MakeObjStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                          sbe::value::SlotId{1},
                                          sbe::value::SlotId{2},
                                          sbe::MakeObjStage::FieldBehavior::keep,
                                          std::vector<std::string>{"kept", "fields"},
                                          std::vector<std::string>{"projected", "fields"},
                                          sbe::makeSV(3, 4),
                                          false,
                                          false,
                                          planNodeId),
            sbe::makeS<sbe::MakeObjStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                          sbe::value::SlotId{1},
                                          boost::none,
                                          boost::none,
                                          std::vector<std::string>{},
                                          std::vector<std::string>{"projected", "fields"},
                                          sbe::makeSV(3, 4),
                                          false,
                                          false,
                                          planNodeId),
            sbe::makeS<sbe::MakeBsonObjStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                              sbe::value::SlotId{1},
                                              boost::none,
                                              boost::none,
                                              std::vector<std::string>{},
                                              std::vector<std::string>{"projected", "fields"},
                                              sbe::makeSV(3, 4),
                                              false,
                                              false,
                                              planNodeId),
            sbe::makeS<sbe::MakeBsonObjStage>(sbe::makeS<sbe::CoScanStage>(planNodeId),
                                              sbe::value::SlotId{1},
                                              sbe::value::SlotId{2},
                                              sbe::MakeBsonObjStage::FieldBehavior::drop,
                                              std::vector<std::string>{"restricted", "fields"},
                                              std::vector<std::string>{"projected", "fields"},
                                              sbe::makeSV(3, 4),
                                              false,
                                              false,
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
                    sbe::PlanStage::Vector ret;
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
                    sbe::PlanStage::Vector ret;
                    ret.push_back(sbe::makeS<sbe::CoScanStage>(planNodeId));
                    ret.push_back(sbe::makeS<sbe::CoScanStage>(planNodeId));
                    return ret;
                }()),
                std::vector<sbe::value::SlotVector>{sbe::makeSV(1, 2), sbe::makeSV(3, 4)},
                std::vector<sbe::value::SortDirection>{sbe::value::SortDirection::Ascending,
                                                       sbe::value::SortDirection::Ascending},
                std::vector<sbe::value::SlotVector>{sbe::makeSV(1, 2), sbe::makeSV(3, 4)},
                sbe::makeSV(5, 6),
                planNodeId));
    }

    PlanNodeId planNodeId;
    sbe::PlanStage::Vector stages;
};

TEST_F(SBEParserTest, TestIdenticalDebugOutputAfterParse) {
    sbe::DebugPrinter printer;

    for (const auto& stage : stages) {
        auto env = std::make_unique<sbe::RuntimeEnvironment>();
        sbe::Parser parser(env.get());
        const auto stageText = printer.print(*stage);

        const auto parsedStage = parser.parse(nullptr, "testDb", stageText);
        const auto stageTextAfterParse = printer.print(*parsedStage);

        ASSERT_EQ(normalizeSbePlanString(stageText), normalizeSbePlanString(stageTextAfterParse));
    }
}

TEST_F(SBEParserTest, TestPlanNodeIdIsParsed) {
    sbe::DebugPrinter printer;
    auto env = std::make_unique<sbe::RuntimeEnvironment>();
    sbe::Parser parser(env.get());

    for (const auto& stage : stages) {
        const auto stageText = printer.print(*stage);
        const auto parsedStage = parser.parse(nullptr, "testDb", stageText);
        ASSERT_EQ(parsedStage->getCommonStats()->nodeId, planNodeId);
    }
}

}  // namespace
}  // namespace mongo
