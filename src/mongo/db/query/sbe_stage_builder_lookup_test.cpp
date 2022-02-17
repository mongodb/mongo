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
 * This file contains tests for building execution stages that implement $lookup operator.
 */

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/util/assert_util.h"

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"

namespace mongo::sbe {
using namespace value;

struct LookupStageBuilderTest : public SbeStageBuilderTestFixture {
    // VirtualScanNode wants the docs wrapped into BSONArray.
    std::vector<BSONArray> prepInputForVirtualScanNode(const std::vector<BSONObj>& docs) {
        std::vector<BSONArray> input;
        input.reserve(docs.size());
        for (const auto& doc : docs) {
            input.emplace_back(BSON_ARRAY(doc));
        }
        return input;
    }

    // Constructs a QuerySolution consisting of a EqLookupNode on top of two VirtualScanNodes.
    std::unique_ptr<QuerySolution> makeLookupSolution(const std::string& lookupSpec,
                                                      const std::string& fromColl,
                                                      const std::vector<BSONObj>& localDocs,
                                                      const std::vector<BSONObj>& foreignDocs) {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        auto lookupNss = NamespaceString{"test", fromColl};
        expCtx->setResolvedNamespace(lookupNss,
                                     ExpressionContext::ResolvedNamespace{lookupNss, {}});

        auto docSource =
            DocumentSourceLookUp::createFromBson(fromjson(lookupSpec).firstElement(), expCtx);
        auto docLookup = static_cast<DocumentSourceLookUp*>(docSource.get());

        auto localVirtScanNode =
            std::make_unique<VirtualScanNode>(prepInputForVirtualScanNode(localDocs),
                                              VirtualScanNode::ScanType::kCollScan,
                                              false /*hasRecordId*/);

        auto lookupNode = std::make_unique<EqLookupNode>(std::move(localVirtScanNode),
                                                         fromColl,
                                                         docLookup->getLocalField()->fullPath(),
                                                         docLookup->getForeignField()->fullPath(),
                                                         docLookup->getAsField().fullPath());

        auto foreignVirtScanNode =
            std::make_unique<VirtualScanNode>(prepInputForVirtualScanNode(foreignDocs),
                                              VirtualScanNode::ScanType::kCollScan,
                                              false /*hasRecordId*/);
        std::vector<std::unique_ptr<QuerySolutionNode>> additionalChildren;
        additionalChildren.push_back(std::move(foreignVirtScanNode));
        lookupNode->addChildren(std::move(additionalChildren));

        return makeQuerySolution(std::move(lookupNode));
    }

    // Execute the stage tree and check the results.
    void CheckNljResults(PlanStage* nljStage,
                         SlotId localSlot,
                         SlotId matchedSlot,
                         const std::vector<std::pair<BSONObj, std::vector<BSONObj>>>& expected,
                         bool debugPrint = false) {

        auto ctx = makeCompileCtx();
        prepareTree(ctx.get(), nljStage);
        SlotAccessor* outer = nljStage->getAccessor(*ctx, localSlot);
        SlotAccessor* inner = nljStage->getAccessor(*ctx, matchedSlot);

        size_t i = 0;
        for (auto st = nljStage->getNext(); st == PlanState::ADVANCED;
             st = nljStage->getNext(), i++) {
            auto [outerTag, outerVal] = outer->copyOrMoveValue();
            ValueGuard outerGuard{outerTag, outerVal};
            if (debugPrint) {
                std::cout << i << " outer: " << std::make_pair(outerTag, outerVal) << std::endl;
            }
            if (i >= expected.size()) {
                // We'll assert eventually that there were more actual results than expected.
                continue;
            }

            auto [expectedOuterTag, expectedOuterVal] = copyValue(
                TypeTags::bsonObject, bitcastFrom<const char*>(expected[i].first.objdata()));
            ValueGuard expectedOuterGuard{expectedOuterTag, expectedOuterVal};

            assertValuesEqual(outerTag, outerVal, expectedOuterTag, expectedOuterVal);

            auto [innerTag, innerVal] = inner->copyOrMoveValue();
            ValueGuard innerGuard{innerTag, innerVal};

            ASSERT_EQ(innerTag, TypeTags::Array);
            auto innerMatches = getArrayView(innerVal);

            if (debugPrint) {
                std::cout << "  inner:" << std::endl;
                for (size_t m = 0; m < innerMatches->size(); m++) {
                    auto [matchedTag, matchedVal] = innerMatches->getAt(m);
                    std::cout << "  " << m << ": " << std::make_pair(matchedTag, matchedVal)
                              << std::endl;
                }
            }

            ASSERT_EQ(innerMatches->size(), expected[i].second.size());
            for (size_t m = 0; m < innerMatches->size(); m++) {
                auto [matchedTag, matchedVal] = innerMatches->getAt(m);
                auto [expectedMatchTag, expectedMatchVal] =
                    copyValue(TypeTags::bsonObject,
                              bitcastFrom<const char*>(expected[i].second[m].objdata()));
                ValueGuard expectedMatchGuard{expectedMatchTag, expectedMatchVal};
                assertValuesEqual(matchedTag, matchedVal, expectedMatchTag, expectedMatchVal);
            }
        }
        ASSERT_EQ(i, expected.size());
        nljStage->close();
    }

    void runTest(const std::vector<BSONObj>& ldocs,
                 const std::vector<BSONObj>& fdocs,
                 const std::string& lkey,
                 const std::string& fkey,
                 const std::vector<std::pair<BSONObj, std::vector<BSONObj>>>& expected,
                 bool debugPrint = false) {
        const char* foreignCollName = "fromColl";
        std::stringstream lookupSpec;
        lookupSpec << "{$lookup: ";
        lookupSpec << "  {";
        lookupSpec << "    from: '" << foreignCollName << "', ";
        lookupSpec << "    localField:   '" << lkey << "', ";
        lookupSpec << "    foreignField: '" << fkey << "', ";
        lookupSpec << "    as: 'matched'";
        lookupSpec << "  }";
        lookupSpec << "}";

        auto solution = makeLookupSolution(lookupSpec.str(), foreignCollName, ldocs, fdocs);
        auto [resultSlots, stage, data, _] = buildPlanStage(std::move(solution),
                                                            false /*hasRecordId*/,
                                                            nullptr /*shard filterer*/,
                                                            nullptr /*collator*/);
        if (debugPrint) {
            debugPrintPlan(*stage);
        }

        CheckNljResults(stage.get(),
                        data.outputs.get("local"),
                        data.outputs.get("matched"),
                        expected,
                        debugPrint);
    }

    void debugPrintPlan(const PlanStage& stage, StringData header = "") {
        std::cout << std::endl << "*** " << header << " ***" << std::endl;
        std::cout << DebugPrinter{}.print(stage.debugPrint());
        std::cout << std::endl;
    }
};

TEST_F(LookupStageBuilderTest, NestedLoopJoin_Basic) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{id:0, lkey:1}"),
        fromjson("{id:1, lkey:12}"),
        fromjson("{id:2, lkey:3}"),
        fromjson("{id:3, lkey:[1,4]}"),
    };

    const std::vector<BSONObj> fdocs = {
        fromjson("{id:0, fkey:1}"),
        fromjson("{id:1, fkey:3}"),
        fromjson("{id:2, fkey:[1,4,25]}"),
        fromjson("{id:3, fkey:4}"),
        fromjson("{id:4, fkey:[24,25,26]}"),
        fromjson("{id:5, no_fkey:true}"),
        fromjson("{id:6, fkey:null}"),
        fromjson("{id:7, fkey:undefined}"),
        fromjson("{id:8, fkey:[]}"),
        fromjson("{id:9, fkey:[null]}"),
    };

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0], fdocs[2]}},
        {ldocs[1], {}},
        {ldocs[2], {fdocs[1]}},
        {ldocs[3], {fdocs[0], fdocs[2], fdocs[3]}},
    };

    runTest(ldocs, fdocs, "lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_LocalKey_Null) {
    const std::vector<BSONObj> ldocs = {fromjson("{id:0, lkey:null}")};

    const std::vector<BSONObj> fdocs = {fromjson("{id:0, fkey:1}"),
                                        fromjson("{id:1, no_fkey:true}"),
                                        fromjson("{id:2, fkey:null}"),
                                        fromjson("{id:3, fkey:[null]}"),
                                        fromjson("{id:4, fkey:undefined}"),
                                        fromjson("{id:5, fkey:[undefined]}"),
                                        fromjson("{id:6, fkey:[]}"),
                                        fromjson("{id:7, fkey:[[]]}")};

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected{
        {ldocs[0], {fdocs[1], fdocs[2], fdocs[3], fdocs[4], fdocs[5]}},
    };

    runTest(ldocs, fdocs, "lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_LocalKey_Missing) {
    const std::vector<BSONObj> ldocs = {fromjson("{id:0, no_lkey:true}")};

    const std::vector<BSONObj> fdocs = {fromjson("{id:0, fkey:1}"),
                                        fromjson("{id:1, no_fkey:true}"),
                                        fromjson("{id:2, fkey:null}"),
                                        fromjson("{id:3, fkey:[null]}"),
                                        fromjson("{id:4, fkey:undefined}"),
                                        fromjson("{id:5, fkey:[undefined]}"),
                                        fromjson("{id:6, fkey:[]}"),
                                        fromjson("{id:7, fkey:[[]]}")};

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[1], fdocs[2], fdocs[3], fdocs[4], fdocs[5]}},
    };

    runTest(ldocs, fdocs, "lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_EmptyArrays) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{id:0, lkey:[]}"),
        fromjson("{id:1, lkey:[[]]}"),
    };
    const std::vector<BSONObj> fdocs = {
        fromjson("{id:0, fkey:1}"),
        fromjson("{id:1, no_fkey:true}"),
        fromjson("{id:2, fkey:null}"),
        fromjson("{id:3, fkey:[null]}"),
        fromjson("{id:4, fkey:undefined}"),
        fromjson("{id:5, fkey:[undefined]}"),
        fromjson("{id:6, fkey:[]}"),
        fromjson("{id:7, fkey:[[]]}"),
    };

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {}},          // TODO SERVER-63368: fix this case if the ticket is declined
        {ldocs[1], {fdocs[7]}},  // TODO SEVER-63700: it should be {fdocs[6], fdocs[7]}
    };

    runTest(ldocs, fdocs, "lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_LocalKey_SubFieldScalar) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{id:0, nested:{lkey:1, other:3}}"),
        fromjson("{id:1, nested:{no_lkey:true}}"),
        fromjson("{id:2, nested:1}"),
        fromjson("{id:3, lkey:1}"),
        fromjson("{id:4, nested:{lkey:42}}"),
    };
    const std::vector<BSONObj> fdocs = {
        fromjson("{id:0, fkey:1}"),
        fromjson("{id:1, no_fkey:true}"),
        fromjson("{id:2, fkey:3}"),
        fromjson("{id:3, fkey:[1, 2]}"),
    };

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0], fdocs[3]}},
        {ldocs[1], {fdocs[1]}},
        {ldocs[2], {fdocs[1]}},
        {ldocs[3], {fdocs[1]}},
        {ldocs[4], {}},
    };

    // TODO SERVER-63690: enable this test.
    // runTest(ldocs, fdocs, "nested.lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_LocalKey_SubFieldArray) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{id:0, nested:[{lkey:1},{lkey:2}]}"),
        fromjson("{id:1, nested:[{lkey:42}]}"),
        fromjson("{id:2, nested:[{lkey:{other:1}}]}"),
        fromjson("{id:3, nested:[{lkey:[]}]}"),
        fromjson("{id:4, nested:[{other:3}]}"),
        fromjson("{id:5, nested:[]}"),
        fromjson("{id:6, nested:[[]]}"),
        fromjson("{id:7, lkey:[1,2]}"),
    };
    const std::vector<BSONObj> fdocs = {
        fromjson("{id:0, fkey:1}"),
        fromjson("{id:1, fkey:2}"),
        fromjson("{id:2, fkey:3}"),
        fromjson("{id:3, fkey:[1, 4]}"),
        fromjson("{id:4, no_fkey:true}"),
        fromjson("{id:5, fkey:[]}"),
        fromjson("{id:6, fkey:null}"),
    };

    //'expected' documents pre-SERVER-63368 behavior of the classic engine.
    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0], fdocs[1], fdocs[3]}},
        {ldocs[1], {}},
        {ldocs[2], {}},
        {ldocs[3], {fdocs[4], fdocs[6]}},
        {ldocs[4], {fdocs[4], fdocs[6]}},
        {ldocs[5], {fdocs[4], fdocs[6]}},
        {ldocs[6], {fdocs[4], fdocs[6]}},
        {ldocs[7], {fdocs[4], fdocs[6]}},
    };

    // TODO SERVER-63690: enable this test.
    // runTest(ldocs, fdocs, "nested.lkey", "fkey", expected, true);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_LocalKey_PathWithNumber) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{id:0, nested:[{lkey:1},{lkey:2}]}"),
        fromjson("{id:1, nested:[{lkey:[2,3,1]}]}"),
        fromjson("{id:2, nested:[{lkey:2},{lkey:1}]}"),
        fromjson("{id:3, nested:[{lkey:[2,3]}]}"),
        fromjson("{id:4, nested:{lkey:1}}"),
        fromjson("{id:5, nested:{lkey:[1,2]}}"),
        fromjson("{id:6, nested:[{other:1},{lkey:1}]}"),
    };
    const std::vector<BSONObj> fdocs = {
        fromjson("{id:0, fkey:1}"),
        fromjson("{id:1, fkey:3}"),
        fromjson("{id:2, fkey:null}"),
    };
    //'expected' documents pre-SERVER-63368 behavior of the classic engine.
    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0]}},
        {ldocs[1], {fdocs[0], fdocs[1]}},
        {ldocs[2], {}},
        {ldocs[3], {fdocs[1]}},
        {ldocs[4], {fdocs[2]}},
        {ldocs[5], {fdocs[2]}},
        {ldocs[6], {fdocs[2]}},
    };

    // TODO SERVER-63690: either remove or enable this test.
    // runTest(ldocs, fdocs, "nested.0.lkey", "fkey", expected, true);
}
}  // namespace mongo::sbe
