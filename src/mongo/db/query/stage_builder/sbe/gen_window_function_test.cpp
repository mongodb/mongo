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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/query/stage_builder/sbe/sbe_builder_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {


class SBESetWindowFieldsTest : public SbeStageBuilderTestFixture {
public:
    boost::intrusive_ptr<DocumentSource> createSetWindowFieldsDocumentSource(
        boost::intrusive_ptr<ExpressionContext> expCtx, const BSONObj& windowSpec) {
        ASSERT(expCtx) << "expCtx must not be null";
        BSONObj spec = BSON("$_internalSetWindowFields" << windowSpec);

        auto docSrc =
            DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), expCtx);
        docSrc->optimize();

        return docSrc;
    }

    std::pair<std::unique_ptr<QuerySolution>,
              boost::intrusive_ptr<DocumentSourceInternalSetWindowFields>>
    makeSetWindowFieldsQuerySolution(boost::intrusive_ptr<ExpressionContext> expCtx,
                                     BSONObj spec,
                                     std::vector<BSONArray> inputDocs) {

        auto docSrcA = createSetWindowFieldsDocumentSource(expCtx, spec);
        auto docSrc = dynamic_cast<DocumentSourceInternalSetWindowFields*>(docSrcA.get());
        ASSERT(docSrc != nullptr);

        // Constructs a QuerySolution consisting of a WindowNode on top of a VirtualScanNode.
        auto virtScanNode = std::make_unique<VirtualScanNode>(
            inputDocs, VirtualScanNode::ScanType::kCollScan, false /*hasRecordId*/);

        auto windowNode = std::make_unique<WindowNode>(std::move(virtScanNode),
                                                       docSrc->getPartitionBy(),
                                                       docSrc->getSortBy(),
                                                       docSrc->getOutputFields());

        // Makes a QuerySolution from the root window node.
        return {makeQuerySolution(std::move(windowNode)), docSrc};
    }

    std::pair<sbe::value::TypeTags, sbe::value::Value> getSetWindowFieldsResults(
        BSONObj windowSpec,
        std::vector<BSONArray> inputDocs,
        std::unique_ptr<CollatorInterface> collator = nullptr) {
        // Makes a QuerySolution for SetWindowFieldsNode over VirtualScanNode.
        auto [querySolution, windowNode] =
            makeSetWindowFieldsQuerySolution(make_intrusive<ExpressionContextForTest>(),
                                             std::move(windowSpec),
                                             std::move(inputDocs));

        // Translates the QuerySolution tree to a sbe::PlanStage tree.
        auto [resultSlots, stage, data, _] = buildPlanStage(
            std::move(querySolution), false /*hasRecordId*/, nullptr, std::move(collator));
        ASSERT_EQ(resultSlots.size(), 1);

        auto resultAccessors = prepareTree(&data.env.ctx, stage.get(), resultSlots[0]);
        return getAllResults(stage.get(), &resultAccessors[0]);
    }

    void runSetWindowFieldsTest(StringData windowSpec,
                                std::vector<BSONArray> inputDocs,
                                const mongo::BSONArray& expectedValue,
                                std::unique_ptr<CollatorInterface> collator = nullptr) {
        auto [resultsTag, resultsVal] = getSetWindowFieldsResults(
            fromjson(windowSpec.rawData()), inputDocs, std::move(collator));
        sbe::value::ValueGuard resultGuard{resultsTag, resultsVal};

        auto [expectedTag, expectedVal] = stage_builder::makeValue(expectedValue);
        sbe::value::ValueGuard expectedGuard{expectedTag, expectedVal};

        ASSERT_TRUE(
            PlanStageTestFixture::valueEquals(resultsTag, resultsVal, expectedTag, expectedVal))
            << "expected: " << std::make_pair(expectedTag, expectedVal)
            << " but got: " << std::make_pair(resultsTag, resultsVal);
    }
};

TEST_F(SBESetWindowFieldsTest, FirstTestPositiveWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {first: {$first: '$b', window: {documents: [1, 2]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "first" << 3)
                   << BSON("a" << 2 << "b" << 3 << "first" << 5)
                   << BSON("a" << 3 << "b" << 5 << "first" << 7)
                   << BSON("a" << 4 << "b" << 7 << "first" << BSONNULL)));
}

TEST_F(SBESetWindowFieldsTest, FirstTestConstantValuePositiveWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {first: {$first: 1000, window: {documents: [1, 2]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "first" << 1000)
                   << BSON("a" << 2 << "b" << 3 << "first" << 1000)
                   << BSON("a" << 3 << "b" << 5 << "first" << 1000)
                   << BSON("a" << 4 << "b" << 7 << "first" << BSONNULL)));
}

TEST_F(SBESetWindowFieldsTest, FirstTestNegativeWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {first: {$first: '$b', window: {documents: [-2, -1]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "first" << BSONNULL)
                   << BSON("a" << 2 << "b" << 3 << "first" << 1)
                   << BSON("a" << 3 << "b" << 5 << "first" << 1)
                   << BSON("a" << 4 << "b" << 7 << "first" << 3)));
}

TEST_F(SBESetWindowFieldsTest, FirstTestConstantValueNegativeWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {first: {$first: 1000, window: {documents: [-2, -1]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "first" << BSONNULL)
                   << BSON("a" << 2 << "b" << 3 << "first" << 1000)
                   << BSON("a" << 3 << "b" << 5 << "first" << 1000)
                   << BSON("a" << 4 << "b" << 7 << "first" << 1000)));
}

TEST_F(SBESetWindowFieldsTest, LastTestPositiveWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {last: {$last: '$b', window: {documents: [1, 2]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "last" << 5)
                   << BSON("a" << 2 << "b" << 3 << "last" << 7)
                   << BSON("a" << 3 << "b" << 5 << "last" << 7)
                   << BSON("a" << 4 << "b" << 7 << "last" << BSONNULL)));
}

TEST_F(SBESetWindowFieldsTest, LastTestConstantValuePositiveWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {last: {$last: 1000, window: {documents: [1, 2]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "last" << 1000)
                   << BSON("a" << 2 << "b" << 3 << "last" << 1000)
                   << BSON("a" << 3 << "b" << 5 << "last" << 1000)
                   << BSON("a" << 4 << "b" << 7 << "last" << BSONNULL)));
}

TEST_F(SBESetWindowFieldsTest, LastTestNegativeWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {last: {$last: '$b', window: {documents: [-2, -1]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "last" << BSONNULL)
                   << BSON("a" << 2 << "b" << 3 << "last" << 1)
                   << BSON("a" << 3 << "b" << 5 << "last" << 3)
                   << BSON("a" << 4 << "b" << 7 << "last" << 5)));
}

TEST_F(SBESetWindowFieldsTest, LastTestConstantValueNegativeWindow) {
    auto docs = std::vector<BSONArray>{BSON_ARRAY(BSON("a" << 1 << "b" << 1)),
                                       BSON_ARRAY(BSON("a" << 2 << "b" << 3)),
                                       BSON_ARRAY(BSON("a" << 3 << "b" << 5)),
                                       BSON_ARRAY(BSON("a" << 4 << "b" << 7))};
    runSetWindowFieldsTest(
        R"({sortBy: {a: 1}, output: {last: {$last: 1000, window: {documents: [-2, -1]} }}})",
        docs,
        BSON_ARRAY(BSON("a" << 1 << "b" << 1 << "last" << BSONNULL)
                   << BSON("a" << 2 << "b" << 3 << "last" << 1000)
                   << BSON("a" << 3 << "b" << 5 << "last" << 1000)
                   << BSON("a" << 4 << "b" << 7 << "last" << 1000)));
}

}  // namespace mongo
