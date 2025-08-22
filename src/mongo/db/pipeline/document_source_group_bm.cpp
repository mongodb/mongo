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

#include "mongo/db/pipeline/document_source_group.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

class DocumentSourceGroupBMFixture : public benchmark::Fixture {
public:
    DocumentSourceGroupBMFixture();

    void runDocumentSourceGroup(int numGroups, int countPerGroup, benchmark::State& state);

protected:
    BSONObj _groupObj;
};

DocumentSourceGroupBMFixture::DocumentSourceGroupBMFixture() {
    _groupObj = fromjson(R"(
        { $group: {
            _id: {
                x: '$x',
                y: '$y'
            },
            sumAccum: {
                $sum: {
                    $multiply: ['$a', '$b']
                }
            },
            firstAccum: {
                $first: '$c'
            }
        }})");
}

void DocumentSourceGroupBMFixture::runDocumentSourceGroup(int numGroups,
                                                          int countPerGroup,
                                                          benchmark::State& state) {
    QueryTestServiceContext qtServiceContext;
    auto opContext = qtServiceContext.makeOperationContext();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "bm");
    auto expCtx = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);

    for (auto keepRunning : state) {
        state.PauseTiming();
        auto group = DocumentSourceGroup::createFromBsonWithMaxMemoryUsage(
            _groupObj.firstElement(), expCtx, std::numeric_limits<int64_t>::max());
        auto groupStage = exec::agg::buildStage(group);
        auto mock = exec::agg::MockStage::createForTest({}, expCtx);
        for (int i = 1; i <= numGroups; ++i) {
            BSONObj obj = BSON("a" << i << "b" << i + 1 << "c" << 10 << "x" << i << "y" << i * 10);
            mock->push_back(Document{obj}, countPerGroup);
        }
        groupStage->setSource(mock.get());
        state.ResumeTiming();
        // Call getNext() only once to ready all the groups. We do not read all the output documents
        // since we only want to benchmark the code that prepares all the groups.
        ASSERT_TRUE(groupStage->getNext().isAdvanced());
    }
}

BENCHMARK_F(DocumentSourceGroupBMFixture, BM_DSGroupBuildPhase100KDocsPerGroup)
(benchmark::State& state) {
    runDocumentSourceGroup(/*numGroups*/ 5, /*countPerGroup*/ 100000, state);
}

BENCHMARK_F(DocumentSourceGroupBMFixture, BM_DSGroupBuildPhase1KDocsPerGroup)
(benchmark::State& state) {
    runDocumentSourceGroup(/*numGroups*/ 500, /*countPerGroup*/ 1000, state);
}

BENCHMARK_F(DocumentSourceGroupBMFixture, BM_DSGroupBuildPhase100DocsPerGroup)
(benchmark::State& state) {
    runDocumentSourceGroup(/*numGroups*/ 5000, /*countPerGroup*/ 100, state);
}

BENCHMARK_F(DocumentSourceGroupBMFixture, BM_DSGroupBuildPhase10DocsPerGroup)
(benchmark::State& state) {
    runDocumentSourceGroup(/*numGroups*/ 50000, /*countPerGroup*/ 10, state);
}

BENCHMARK_F(DocumentSourceGroupBMFixture, BM_DSGroupBuildPhase1DocPerGroup)
(benchmark::State& state) {
    runDocumentSourceGroup(/*numGroups*/ 500000, /*countPerGroup*/ 1, state);
}

}  // namespace
}  // namespace mongo
