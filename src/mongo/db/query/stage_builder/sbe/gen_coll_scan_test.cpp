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

#include "mongo/db/query/stage_builder/sbe/gen_coll_scan.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/stages/clustered_scan_stage_test_fixtures.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/generic_scan.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/multi_range_clustered_scan_stage.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/record_id_range.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_mock.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace mongo::sbe {

class GenCollScanTest : public ClusteredScanStageTestFixture {
public:
    GenCollScanTest()
        : ClusteredScanStageTestFixture(
              NamespaceString::createNamespaceString_forTest("testdb.gen_coll_scan_test")) {}

    // Run a clustered collection scan via generateCollScan.
    // Returns the documents in the order the stage produced them.
    // Checks that the builder has built the correct stage type.
    std::vector<BSONObj> runClusteredScan(const MultipleCollectionAccessor& colls,
                                          const RecordIdRangeList& rangeList,
                                          bool forward) {
        // The plan we construct does not need the virtual scan from runTest, so create these
        // dummy slots.
        auto [emptyTag, emptyVal] = value::makeNewArray();
        auto data = std::make_unique<stage_builder::PlanStageStaticData>();
        auto [resultsTag, resultsVal] = runTest(
            emptyTag,
            emptyVal,
            [&](value::SlotId, std::unique_ptr<PlanStage>, stage_builder::Environment& env)
                -> std::pair<value::SlotId, std::unique_ptr<PlanStage>> {
                Variables variables;
                boost::intrusive_ptr<ExpressionContext> expCtx(
                    new ExpressionContextForTest(operationContext(), _nss));
                value::FrameIdGenerator frameIdGenerator;

                stage_builder::StageBuilderState builderState{operationContext(),
                                                              env,
                                                              data.get(),
                                                              variables,
                                                              getYieldPolicy(),
                                                              getSlotIdGenerator(),
                                                              &frameIdGenerator,
                                                              nullptr /* spoolIdGenerator */,
                                                              nullptr /* inListsMap */,
                                                              nullptr /* collatorsMap */,
                                                              nullptr /* sortSpecMap */,
                                                              expCtx,
                                                              false /* needsMerge */,
                                                              false /* allowDiskUse */,
                                                              *expCtx->getIfrContext()};

                auto csn = std::make_unique<CollectionScanNode>();
                csn->nss = _nss;
                csn->direction =
                    forward ? CollectionScanParams::FORWARD : CollectionScanParams::BACKWARD;
                csn->rangeList = rangeList;
                csn->isClustered = true;

                const CollectionPtr& collection = colls.getMainCollection();
                auto [stage, slots] =
                    stage_builder::generateCollScan(builderState, collection, csn.get(), {});

                // Check that single range scans use ScanStage for bounded scans and
                // GenericScanStage for unbounded scans, whereas zero or multi-range scans use
                // MultiRangeClusteredScanStage.
                if (rangeList.getRanges().size() == 0) {
                    auto limit = dynamic_cast<LimitSkipStage*>(stage.get());
                    ASSERT(limit);

                } else if (rangeList.getRanges().size() == 1) {
                    if (rangeList.isUnbounded()) {
                        ASSERT(dynamic_cast<GenericScanStage*>(stage.get()));
                    } else {
                        ASSERT(dynamic_cast<ScanStage*>(stage.get()));
                    }
                    ASSERT_EQ(dynamic_cast<MultiRangeClusteredScanStage*>(stage.get()), nullptr);
                } else {
                    ASSERT(dynamic_cast<MultiRangeClusteredScanStage*>(stage.get()));
                    ASSERT_EQ(dynamic_cast<ScanStage*>(stage.get()), nullptr);
                }

                env.ctx.mca = &colls;

                return {slots.getResultObj().getId(), std::move(stage)};
            });

        value::TagValueOwned guard = value::TagValueOwned::fromRaw(resultsTag, resultsVal);
        std::vector<BSONObj> results;
        auto* arrView = value::getArrayView(resultsVal);
        for (size_t i = 0; i < arrView->size(); ++i) {
            auto [tag, val] = arrView->getAt(i);
            ASSERT_EQ(tag, value::TypeTags::bsonObject);
            results.push_back(BSONObj(value::getRawPointerView(val)).getOwned());
        }
        return results;
    }

    void checkClusteredScanResult(const MultipleCollectionAccessor& colls,
                                  bool forward,
                                  const RecordIdRangeList& rangeList,
                                  const std::vector<BSONObj>& docs) {

        auto actualDocs = runClusteredScan(colls, rangeList, forward);

        std::vector<BSONObj> sortedDocs = docs;
        std::sort(
            sortedDocs.begin(), sortedDocs.end(), [forward](const BSONObj& a, const BSONObj& b) {
                return forward ? (a["_id"].Int() < b["_id"].Int())
                               : (b["_id"].Int() < a["_id"].Int());
            });

        std::vector<BSONObj> expectedDocs;
        for (const auto& doc : sortedDocs) {
            RecordId rid = record_id_helpers::keyForElem(doc["_id"]);
            bool inAnyRange = false;
            for (const auto& range : rangeList.getRanges()) {
                if (range.compare(rid) == 0) {
                    inAnyRange = true;
                    break;
                }
            }
            if (!inAnyRange)
                continue;
            expectedDocs.push_back(doc);
        }

        ASSERT_EQ(actualDocs.size(), expectedDocs.size())
            << "wrong number of results for " << (forward ? "FORWARD" : "BACKWARD") << " scan";
        for (size_t i = 0; i < actualDocs.size(); ++i) {
            ASSERT_BSONOBJ_EQ(actualDocs[i], expectedDocs[i]);
        }
    }
};

// Tests unbounded clustered collection scans.
TEST_F(GenCollScanTest, ClusteredUnbounded) {
    std::vector<BSONObj> docs;
    for (int i = 0; i < 30; ++i)
        docs.push_back(BSON("_id" << i));
    auto colls = createClusteredCollection(docs);

    // Unbounded by default.
    RecordIdRangeList rangeList;

    for (bool forward : {false, true}) {
        checkClusteredScanResult(colls, forward, rangeList, docs);
    }
}

// Forward and backward scans over a single contiguous range.
TEST_F(GenCollScanTest, ClusteredSingleRange) {
    std::vector<BSONObj> docs;
    for (int i = 0; i < 30; ++i)
        docs.push_back(BSON("_id" << i));
    auto colls = createClusteredCollection(docs);

    auto rangeList = RecordIdRangeList(makeIntRange(1, true, 5, true));  // [1,5]

    checkClusteredScanResult(colls, true, rangeList, docs);
    checkClusteredScanResult(colls, false, rangeList, docs);
}

// Forward and backward scans over multiple non-contiguous ranges.
TEST_F(GenCollScanTest, ClusteredMultiRange) {
    std::vector<BSONObj> docs;
    for (int i = 0; i < 30; ++i)
        docs.push_back(BSON("_id" << i));
    auto colls = createClusteredCollection(docs);

    auto rangeList = RecordIdRangeList::makeUnion({
        makeIntRange(1, true, 5, true),      // [1,5]
        makeIntRange(10, false, 15, false),  // (10,15)
        makeIntRange(17, true, 17, true),    // [17,17]
        makeIntRange(20, true, 25, false),   // [20,25)
    });

    checkClusteredScanResult(colls, true, rangeList, docs);
    checkClusteredScanResult(colls, false, rangeList, docs);
}

// An empty RecordIdRangeList (∅) — no documents should be returned.
TEST_F(GenCollScanTest, ClusteredEmptyRangeList) {
    std::vector<BSONObj> docs;
    for (int i = 0; i < 10; ++i)
        docs.push_back(BSON("_id" << i));
    auto colls = createClusteredCollection(docs);

    auto rangeList = RecordIdRangeList::unite({});  // empty list (∅)

    auto forward = runClusteredScan(colls, rangeList, true);
    ASSERT_TRUE(forward.empty()) << "expected no documents for empty rangeList (forward)";

    auto backward = runClusteredScan(colls, rangeList, false);
    ASSERT_TRUE(backward.empty()) << "expected no documents for empty rangeList (backward)";
}

// The stage builder adds the top-level fields referenced by the filter to the scan's field list, so
// that the scan extracts them into slots. Those additions must also be visible in the returned
// PlanStageSlots: otherwise the filter finds no kField slot for the field and falls back to
// re-extracting it from the result object with getField(), doing the work twice per document.
TEST_F(GenCollScanTest, GenericScanFilterFieldsAreExposedAsSlots) {
    // This test only inspects the shape of the plan and never runs it, so a mock collection (which
    // just supplies a uuid and a namespace) is enough.
    CollectionMock collMock{UUID::gen(), _nss};
    // The initialization of the CollectionPtr is SAFE. The lifetime of the Mocked Collection
    // instance is managed by the test and guaranteed to be valid for the entire duration of the
    // test.
    auto collection = CollectionPtr::CollectionPtr_UNSAFE(&collMock);

    boost::intrusive_ptr<ExpressionContext> expCtx(
        new ExpressionContextForTest(operationContext(), _nss));
    // 'matchExpr' must outlive the parsed MatchExpression, which points into its buffer rather
    // than owning it.
    auto matchExpr = fromjson("{a: 0}");
    auto filter = MatchExpressionParser::parse(matchExpr, expCtx);
    ASSERT_OK(filter.getStatus());

    auto csn = std::make_unique<CollectionScanNode>();
    csn->nss = _nss;
    csn->direction = CollectionScanParams::FORWARD;
    csn->isClustered = true;
    // An unbounded rangeList is not a clustered scan, so this takes the generic scan path.
    csn->rangeList = RecordIdRangeList();
    ASSERT_TRUE(csn->rangeList.isUnbounded());
    csn->filter = std::move(filter.getValue());

    stage_builder::Environment env{std::make_unique<RuntimeEnvironment>()};
    auto data = std::make_unique<stage_builder::PlanStageStaticData>();
    Variables variables;
    value::FrameIdGenerator frameIdGenerator;
    stage_builder::StageBuilderState builderState{operationContext(),
                                                  env,
                                                  data.get(),
                                                  variables,
                                                  getYieldPolicy(),
                                                  getSlotIdGenerator(),
                                                  &frameIdGenerator,
                                                  nullptr /* spoolIdGenerator */,
                                                  nullptr /* inListsMap */,
                                                  nullptr /* collatorsMap */,
                                                  nullptr /* sortSpecMap */,
                                                  expCtx,
                                                  false /* needsMerge */,
                                                  false /* allowDiskUse */,
                                                  *expCtx->getIfrContext()};

    // 'a' is referenced only by the filter, not by the requested fields (which are empty here).
    auto [stage, slots] =
        stage_builder::generateCollScan(builderState, collection, csn.get(), {} /* fields */);

    ASSERT((dynamic_cast<FilterStage<false, false>*>(stage.get())));
    ASSERT_TRUE(
        slots.has(std::make_pair(stage_builder::PlanStageSlots::kField, std::string_view{"a"})))
        << "the filter's top-level field 'a' should be exposed as a kField slot";
}

}  // namespace mongo::sbe
