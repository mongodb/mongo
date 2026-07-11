// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata_gen.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context.h"

#include <deque>
#include <memory>
#include <string>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

constexpr int kDocResultStreamType = 0;
constexpr int kMetaResultStreamType = 1;

// The result-payload shape shared by every benchmark case: {_id: i, score: N-i, name: "doc_<i>"}.
BSONObj makePayload(int i, int numDocs) {
    return BSON("_id" << i << "score" << (numDocs - i) << "name" << ("doc_" + std::to_string(i)));
}

// Doc-result envelopes that DRM's inner source stage emits.
std::deque<DocumentSource::GetNextResult> makeWrappedDocsNoMeta(int numDocs) {
    std::deque<DocumentSource::GetNextResult> docs;
    for (int i = 0; i < numDocs; ++i) {
        docs.emplace_back(Document{
            BSON("_streamType" << kDocResultStreamType << "payload" << makePayload(i, numDocs))});
    }
    return docs;
}

// Prepends a metadata envelope {_streamType: 1, payload: {count: N}} to makeWrappedDocsNoMeta.
std::deque<DocumentSource::GetNextResult> makeWrappedDocs(int numDocs) {
    auto docs = makeWrappedDocsNoMeta(numDocs);
    docs.emplace_front(Document{
        BSON("_streamType" << kMetaResultStreamType << "payload" << BSON("count" << numDocs))});
    return docs;
}

// Plain documents, used for baseline.
std::deque<DocumentSource::GetNextResult> makePlainDocs(int numDocs) {
    std::deque<DocumentSource::GetNextResult> docs;
    for (int i = 0; i < numDocs; ++i) {
        docs.emplace_back(Document{makePayload(i, numDocs)});
    }
    return docs;
}

class DrmBMFixture : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override {
        QueryFCVEnvironmentForTest::setUp();
        _serviceContext = std::make_unique<QueryTestScopedGlobalServiceContext>();
        _opCtx = _serviceContext->makeOperationContext();
    }

    void TearDown(benchmark::State& state) override {
        _opCtx.reset();
        _serviceContext.reset();
    }

    // Shared timed loop for every case. makeDocs supplies the inner-source documents and
    // buildPipeline wraps the populated queue into the pipeline under test. Only the drain of
    // document results is timed; queue population and pipeline compilation run under PauseTiming.
    template <typename MakeDocsFn, typename BuildPipelineFn>
    void runCase(int numDocs,
                 benchmark::State& state,
                 MakeDocsFn makeDocs,
                 BuildPipelineFn buildPipeline) {
        const NamespaceString nss =
            NamespaceString::createNamespaceString_forTest("test", "drm_bm");

        // Built once: the documents are identical across iterations and Document is a copy-on-write
        // handle, so each iteration cheaply copies these into a fresh queue instead of rebuilding
        // the BSON. (expCtx and the queue itself must still be rebuilt per iteration — see below.)
        const auto sourceDocs = makeDocs(numDocs);

        for (auto _ : state) {
            state.PauseTiming();

            auto expCtx = make_intrusive<ExpressionContextForTest>(_opCtx.get(), nss);
            auto queue = DocumentSourceQueue::create(expCtx);
            for (const auto& doc : sourceDocs) {
                queue->push_back(doc);
            }

            // buildPipeline expands the DRM stage into its Exchange + $replaceRoot (+ $setVar) form
            // via the REGISTER_AGG_STAGES_MAPPING handler for $_internalDocumentResultsAndMetadata.
            auto execPipeline = exec::agg::buildPipeline(buildPipeline(std::move(queue), expCtx));

            state.ResumeTiming();

            int count = 0;
            while (execPipeline->getNextResult().isAdvanced()) {
                ++count;
            }
            benchmark::DoNotOptimize(count);
        }
        state.SetItemsProcessed(state.iterations() * numDocs);
    }

private:
    std::unique_ptr<QueryTestScopedGlobalServiceContext> _serviceContext;
    ServiceContext::UniqueOperationContext _opCtx;
};

void drmBenchmarkArgs(benchmark::internal::Benchmark* b) {
    b->RangeMultiplier(10)->Range(100, 10000);
}

// Case 1 — DRM with metadata. Expands to
//   [Exchange(consumer=0), $replaceRoot($payload),
//    $setVar(SEARCH_META, sub: [Exchange(consumer=1), $replaceRoot($payload)])]
// the full standalone $search-with-$$SEARCH_META path. $setVar eagerly drains consumer 1 (meta)
// before the first doc, so the timed path includes the meta drain plus N * (routing +
// $replaceRoot).
BENCHMARK_DEFINE_F(DrmBMFixture, BM_DrmWithMeta)(benchmark::State& state) {
    runCase(state.range(0),
            state,
            makeWrappedDocs,
            [](boost::intrusive_ptr<DocumentSource> source,
               const boost::intrusive_ptr<ExpressionContext>& expCtx) {
                auto drm = DocumentSourceInternalDocumentResultsAndMetadata::create(
                    expCtx,
                    std::move(source),
                    MetadataBindSpec("SEARCH_META"),
                    /*returnCursor=*/false);
                return Pipeline::create({std::move(drm)}, expCtx)->freeze();
            });
}
BENCHMARK_REGISTER_F(DrmBMFixture, BM_DrmWithMeta)->Apply(drmBenchmarkArgs);

// Case 2 — DRM with the metadata stream elided. Expands to
//   [Exchange(consumer=0, 1-consumer spec), $replaceRoot($payload)]
// isolating the Exchange + $replaceRoot overhead without the $setVar / metadata-drain cost.
BENCHMARK_DEFINE_F(DrmBMFixture, BM_DrmNoMeta)(benchmark::State& state) {
    runCase(state.range(0),
            state,
            makeWrappedDocsNoMeta,
            [](boost::intrusive_ptr<DocumentSource> source,
               const boost::intrusive_ptr<ExpressionContext>& expCtx) {
                auto drm = DocumentSourceInternalDocumentResultsAndMetadata::create(
                    expCtx, std::move(source), boost::none, /*returnCursor=*/false);
                return Pipeline::create({std::move(drm)}, expCtx)->freeze();
            });
}
BENCHMARK_REGISTER_F(DrmBMFixture, BM_DrmNoMeta)->Apply(drmBenchmarkArgs);

// Case 3 — Direct baseline: a plain queue drain with no Exchange or $replaceRoot wrapping. The
// delta between this and Case 2 is the per-document cost of Exchange(1-consumer) + $replaceRoot.
BENCHMARK_DEFINE_F(DrmBMFixture, BM_DirectPath)(benchmark::State& state) {
    runCase(state.range(0),
            state,
            makePlainDocs,
            [](boost::intrusive_ptr<DocumentSource> source,
               const boost::intrusive_ptr<ExpressionContext>& expCtx) {
                return Pipeline::create({std::move(source)}, expCtx)->freeze();
            });
}
BENCHMARK_REGISTER_F(DrmBMFixture, BM_DirectPath)->Apply(drmBenchmarkArgs);

}  // namespace
}  // namespace mongo
