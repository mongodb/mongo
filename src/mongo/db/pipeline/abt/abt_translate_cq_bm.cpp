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

#include <benchmark/benchmark.h>
#include <memory>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/bonsai_query_bm_fixture.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/abt/canonical_query_translation.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/db/query/query_test_service_context.h"

namespace mongo::optimizer {
namespace {
/**
 * Benchmarks translation from CanonicalQuery to ABT.
 */
class CanonicalQueryABTTranslate : public BonsaiQueryBenchmarkFixture {
public:
    CanonicalQueryABTTranslate() {}

    void benchmarkPipeline(benchmark::State& state, const std::vector<BSONObj>& pipeline) final {
        state.SkipWithError("Find translation fixture cannot translate a pipeline");
        return;
    }

    void benchmarkQueryMatchProject(benchmark::State& state,
                                    BSONObj matchSpec,
                                    BSONObj projectSpec) final {
        QueryTestServiceContext testServiceContext;
        auto opCtx = testServiceContext.makeOperationContext();
        auto nss = NamespaceString::createNamespaceString_forTest("test.bm");

        Metadata metadata{{}};
        auto prefixId = PrefixId::createForTests();
        ProjectionName scanProjName{prefixId.getNextId("scan")};

        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(matchSpec);
        findCommand->setProjection(projectSpec);
        auto cq = std::make_unique<CanonicalQuery>(
            CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx.get(), *findCommand),
                                 .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
        QueryParameterMap qp;

        // This is where recording starts.
        for (auto keepRunning : state) {
            benchmark::DoNotOptimize(
                translateCanonicalQueryToABT(metadata,
                                             *cq,
                                             scanProjName,
                                             make<ScanNode>(scanProjName, "collection"),
                                             prefixId,
                                             qp));
            benchmark::ClobberMemory();
        }
    }
};

BENCHMARK_MQL_TRANSLATION(CanonicalQueryABTTranslate)
}  // namespace
}  // namespace mongo::optimizer
