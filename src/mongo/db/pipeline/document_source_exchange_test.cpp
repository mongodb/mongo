// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/document_source_exchange.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/hasher.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_group.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <mutex>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
using namespace std::literals::string_view_literals;

namespace {
/**
 * This class is used for an Exchange consumer to temporarily relinquish control of a mutex
 * while it's blocked.
 */
class MutexYielder : public ResourceYielder {
public:
    MutexYielder(std::mutex* mutex) : _lock(*mutex, std::defer_lock) {}

    void yield(OperationContext* opCtx) override {
        _lock.unlock();
    }

    void unyield(OperationContext* opCtx) override {
        _lock.lock();
    }

    std::unique_lock<std::mutex>& getLock() {
        return _lock;
    }

private:
    std::unique_lock<std::mutex> _lock;
};

/**
 * Used to keep track of each client and operation context.
 */
struct ThreadInfo {
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;
    boost::intrusive_ptr<DocumentSourceExchange> documentSourceExchange;
    MutexYielder* yielder;
};
}  // namespace

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("test.docSourceExchange"sv);

class DocumentSourceExchangeTest : service_context_test::WithSetupTransportLayer,
                                   public AggregationContextFixture {
protected:
    void setUp() override {
        _executor = executor::ThreadPoolTaskExecutor::create(
            ThreadPool::make({}), executor::makeNetworkInterface("ExchangeTest"));
        _executor->startup();
    }

    void tearDown() override {
        _executor->shutdown();
        _executor.reset();
    }

    static const size_t strValLen = 27;

    auto getMockSource(int cnt) {
        std::vector<Document> docs;
        docs.reserve(cnt);
        std::string strVal{strValLen, 'a'};

        for (int i = 0; i < cnt; ++i)
            docs.emplace_back(Document{{"a", i}, {"b", strVal}});

        return DocumentSourceMock::createForTest(std::move(docs), getExpCtx());
    }

    static auto getNewSeed() {
        auto seed = Date_t::now().asInt64();
        LOGV2(20898, "Generated new seed is {seed}", "seed"_attr = seed);

        return seed;
    }

    auto getRandomMockSource(size_t cnt, int64_t seed) {
        PseudoRandom prng(seed);
        std::vector<Document> docs;
        docs.reserve(cnt);

        std::string strVal{strValLen, 'a'};
        for (size_t i = 0; i < cnt; ++i)
            docs.emplace_back(
                Document{{"a", static_cast<int>(prng.nextInt32() % cnt)}, {"b", strVal}});

        return DocumentSourceMock::createForTest(std::move(docs), getExpCtx());
    }

    auto parseSpec(const BSONObj& spec) {
        IDLParserContext ctx("internalExchange");
        return ExchangeSpec::parse(spec, ctx);
    }

    // Producer pipeline of a mock source feeding $group, which tracks memory.
    std::unique_ptr<Pipeline> makeGroupProducerPipeline(size_t nInputDocs, size_t nOutputDocs) {
        const auto mock = getMockSource(static_cast<int>(nInputDocs));
        BSONObj groupBson = fromjson(fmt::format(R"({{$group: {{
            _id: {{$mod: ["$a",  {}]}},
            v: {{$push: "$b"}}
        }}}})",
                                                 nOutputDocs));
        auto group = DocumentSourceGroup::createFromBson(groupBson["$group"], getExpCtx());
        return Pipeline::create({mock, group}, getExpCtx());
    }

    auto createNProducers(size_t nConsumers, boost::intrusive_ptr<exec::agg::Exchange> ex) {
        std::vector<ThreadInfo> threads;
        for (size_t idx = 0; idx < nConsumers; ++idx) {
            ServiceContext::UniqueClient client =
                getServiceContext()->getService()->makeClient("exchange client");
            ServiceContext::UniqueOperationContext opCtxOwned =
                getServiceContext()->makeOperationContext(client.get());
            OperationContext* opCtx = opCtxOwned.get();
            threads.emplace_back(ThreadInfo{
                std::move(client),
                std::move(opCtxOwned),
                new DocumentSourceExchange(
                    ExpressionContextBuilder{}.opCtx(opCtx).ns(kTestNss).build(), ex, idx, nullptr),
            });
        }
        return threads;
    }

    std::shared_ptr<executor::TaskExecutor> _executor;
};  // namespace mongo

TEST_F(DocumentSourceExchangeTest, SimpleExchange1Consumer) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(1);
    spec.setBufferSize(1024);

    // Upon creation, the Exchange object detaches the pipeline from the operation context, and, as
    // a result, the optCtx on the ExpressionContext is reset to nullptr. So, we need to preserve
    // the opCtx in order to pass it to the getNext call below, which will re-attach the pipeline to
    // the provided opCtx.
    auto opCtx = getOpCtx();
    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(opCtx, spec, Pipeline::create({source}, getExpCtx()));
    auto input = ex->getNext(opCtx, 0, nullptr);

    size_t docs = 0;
    for (; input.isAdvanced(); input = ex->getNext(opCtx, 0, nullptr)) {
        ++docs;
    }

    ASSERT_EQ(docs, nDocs);
}

TEST_F(DocumentSourceExchangeTest, SimpleExchangeNConsumer) {
    const size_t nDocs = 500;
    auto source = getMockSource(500);

    const size_t nConsumers = 5;

    ASSERT_EQ(nDocs % nConsumers, 0u);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(getOpCtx(), spec, Pipeline::create({source}, getExpCtx()));

    std::vector<ThreadInfo> threads = createNProducers(nConsumers, ex);
    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    for (size_t id = 0; id < nConsumers; ++id) {
        auto docSourceExchange = exec::agg::buildStage(threads[id].documentSourceExchange);
        auto handle = _executor->scheduleWork([docSourceExchange, id, nDocs, nConsumers](
                                                  const executor::TaskExecutor::CallbackArgs& cb) {
            PseudoRandom prng(getNewSeed());

            auto input = docSourceExchange->getNext();

            size_t docs = 0;

            for (; input.isAdvanced(); input = docSourceExchange->getNext()) {
                sleepmillis(prng.nextInt32() % 20 + 1);
                ++docs;
            }
            ASSERT_EQ(docs, nDocs / nConsumers);
        });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);
}

TEST_F(DocumentSourceExchangeTest, SimpleExchangeNConsumerMemoryTracking) {
    unittest::ServerParameterGuard featureFlagController("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard curOpWriteBytes("internalQueryMaxWriteToCurOpMemoryUsageBytes",
                                                   64);

    // Create a pipeline that uses group, which will track memory.
    const size_t nInputDocs = 500;
    const size_t nOutputDocs = 250;
    const size_t nConsumers = 5;
    ASSERT_EQ(nOutputDocs % nConsumers, 0u);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<exec::agg::Exchange> ex = new exec::agg::Exchange(
        getOpCtx(), spec, makeGroupProducerPipeline(nInputDocs, nOutputDocs));

    std::vector<ThreadInfo> threads = createNProducers(nConsumers, ex);
    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    for (size_t id = 0; id < nConsumers; ++id) {
        auto docSourceExchange = exec::agg::buildStage(threads[id].documentSourceExchange);
        auto handle = _executor->scheduleWork([docSourceExchange, id, nInputDocs, nConsumers](
                                                  const executor::TaskExecutor::CallbackArgs& cb) {
            PseudoRandom prng(getNewSeed());

            auto input = docSourceExchange->getNext();

            size_t docs = 0;

            for (; input.isAdvanced(); input = docSourceExchange->getNext()) {
                sleepmillis(prng.nextInt32() % 20 + 1);
                ++docs;
            }
            ASSERT_EQ(docs, nOutputDocs / nConsumers);

            // Disposed is actually called twice for the group stage, once from the group stage
            // when EOF is hit, and once here (recursively). Calling it again here ensures that
            // consumer 0 will propagate memory stats up to CurOp.
            docSourceExchange->dispose();

            if (id == 0) {
                // If this is consumer 0, then its CurOp will contain the memory statistics for
                // the subpipeline.
                OperationContext* opCtx = docSourceExchange->getContext()->getOperationContext();
                CurOp* curOp = CurOp::get(opCtx);
                // There may be platform differences in the amount of memory usage, so just
                // assert a reasonable lower bound.
                ASSERT_GT(curOp->getPeakTrackedMemoryBytes(),
                          static_cast<int64_t>(strValLen * nInputDocs));
                // Dispose has been called, so the group stage will have set its in-use metric
                // back to zero.
                ASSERT_EQ(curOp->getInUseTrackedMemoryBytes(), 0);
            }
        });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);
}

TEST_F(DocumentSourceExchangeTest, OwnWithoutReportingTracksProducerMemoryWithoutCurOpReporting) {
    unittest::ServerParameterGuard featureFlagController("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard curOpWriteBytes("internalQueryMaxWriteToCurOpMemoryUsageBytes",
                                                   64);

    // Create a producer pipeline that uses group, which will track memory.
    const size_t nInputDocs = 500;
    const size_t nOutputDocs = 250;

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(1);
    spec.setBufferSize(1024);

    auto opCtx = getOpCtx();
    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(opCtx,
                                spec,
                                makeGroupProducerPipeline(nInputDocs, nOutputDocs),
                                exec::agg::Exchange::InputMemoryPolicy::kOwnWithoutReporting);

    // The Exchange owns the producer's operation tracker, so nothing is left on the opCtx.
    ASSERT_FALSE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));

    // Model a consumer-side memory-tracking stage (as in $_internalDocumentResultsAndMetadata):
    // this creates a fresh operation tracker on the consumer's opCtx that the exchange must not
    // disturb.
    const int64_t consumerBytes = 1000;
    auto consumerTracker = OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForSBE(opCtx);
    consumerTracker.add(consumerBytes);
    ASSERT_TRUE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));
    ASSERT_EQ(CurOp::get(opCtx)->getInUseTrackedMemoryBytes(), consumerBytes);

    size_t docs = 0;
    for (auto input = ex->getNext(opCtx, 0, nullptr); input.isAdvanced();
         input = ex->getNext(opCtx, 0, nullptr)) {
        ++docs;
    }
    ASSERT_EQ(docs, nOutputDocs);
    ex->dispose(opCtx, 0);

    // The producer's memory was genuinely tracked: the Exchange-owned tracker accumulated the
    // group's memory (its peak reflects the pushed strings), even though none of it was reported.
    ASSERT_GT(ex->getOperationMemoryTracker_forTest()->peakTrackedMemoryBytes(),
              static_cast<int64_t>(strValLen * nInputDocs));

    CurOp* curOp = CurOp::get(opCtx);
    // ...but never reported to the consumer's CurOp: the consumer's own stats are untouched.
    ASSERT_EQ(curOp->getPeakTrackedMemoryBytes(), consumerBytes);
    // The consumer's tracker is still in place and functional.
    ASSERT_TRUE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));
    consumerTracker.add(-consumerBytes);
    ASSERT_EQ(curOp->getInUseTrackedMemoryBytes(), 0);
}

TEST_F(DocumentSourceExchangeTest, ShareOperationTrackerReportsProducerMemoryToQueryCurOp) {
    unittest::ServerParameterGuard featureFlagController("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard curOpWriteBytes("internalQueryMaxWriteToCurOpMemoryUsageBytes",
                                                   64);

    // Create a producer pipeline that uses group, which will track memory.
    const size_t nInputDocs = 500;
    const size_t nOutputDocs = 250;

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(1);
    spec.setBufferSize(1024);

    auto opCtx = getOpCtx();
    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(opCtx,
                                spec,
                                makeGroupProducerPipeline(nInputDocs, nOutputDocs),
                                exec::agg::Exchange::InputMemoryPolicy::kShareOperationTracker);

    // The operation tracker stays on the opCtx, shared by the producer (and any consumer stages
    // built on this opCtx).
    ASSERT_TRUE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));

    // A consumer-side stage tracker created on the same opCtx chains to that same shared
    // operation tracker, so producer and consumer memory aggregate into one CurOp.
    const int64_t consumerBytes = 1000;
    auto consumerTracker = OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForSBE(opCtx);
    consumerTracker.add(consumerBytes);
    ASSERT_EQ(CurOp::get(opCtx)->getInUseTrackedMemoryBytes(), consumerBytes);

    size_t docs = 0;
    for (auto input = ex->getNext(opCtx, 0, nullptr); input.isAdvanced();
         input = ex->getNext(opCtx, 0, nullptr)) {
        ++docs;
    }
    ASSERT_EQ(docs, nOutputDocs);
    ex->dispose(opCtx, 0);

    CurOp* curOp = CurOp::get(opCtx);
    // The producer's memory is accounted and reported as part of the whole query, on top of the
    // consumer's contribution held throughout the drain.
    ASSERT_GT(curOp->getPeakTrackedMemoryBytes(),
              static_cast<int64_t>(strValLen * nInputDocs) + consumerBytes);
    // Dispose returned the group's memory, leaving exactly the consumer's contribution in use.
    ASSERT_EQ(curOp->getInUseTrackedMemoryBytes(), consumerBytes);
    consumerTracker.add(-consumerBytes);
    ASSERT_EQ(curOp->getInUseTrackedMemoryBytes(), 0);
    // The tracker still belongs to the operation, not the Exchange.
    ASSERT_TRUE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));
}

TEST_F(DocumentSourceExchangeTest, OwnAndReportRoundTripsTrackerThroughConsumerZeroOpCtx) {
    unittest::ServerParameterGuard featureFlagController("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard curOpWriteBytes("internalQueryMaxWriteToCurOpMemoryUsageBytes",
                                                   64);

    const size_t nInputDocs = 500;
    const size_t nOutputDocs = 250;

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(1);
    spec.setBufferSize(1024);

    auto opCtx = getOpCtx();
    // Default policy: kOwnAndReportToCurOp.
    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(opCtx, spec, makeGroupProducerPipeline(nInputDocs, nOutputDocs));

    // The constructor took the producer's tracker off the opCtx.
    ASSERT_FALSE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));

    size_t docs = 0;
    for (auto input = ex->getNext(opCtx, 0, nullptr); input.isAdvanced();
         input = ex->getNext(opCtx, 0, nullptr)) {
        ++docs;
        // Each getNext publishes the tracker to consumer 0's opCtx while loading and reclaims it
        // on detach; between calls it must be back in the Exchange, not left on the opCtx.
        ASSERT_FALSE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));
    }
    ASSERT_EQ(docs, nOutputDocs);
    ex->dispose(opCtx, 0);

    // The dispose flush also publishes, reports, and reclaims the tracker.
    ASSERT_FALSE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));
    CurOp* curOp = CurOp::get(opCtx);
    ASSERT_GT(curOp->getPeakTrackedMemoryBytes(), static_cast<int64_t>(strValLen * nInputDocs));
    ASSERT_EQ(curOp->getInUseTrackedMemoryBytes(), 0);
}

TEST_F(DocumentSourceExchangeTest, OwnWithoutReportingMultiConsumerThroughStageLayer) {
    unittest::ServerParameterGuard featureFlagController("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard curOpWriteBytes("internalQueryMaxWriteToCurOpMemoryUsageBytes",
                                                   64);

    const size_t nInputDocs = 500;
    const size_t nOutputDocs = 250;
    const size_t nConsumers = 2;
    ASSERT_EQ(nOutputDocs % nConsumers, 0u);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(nConsumers);
    // A buffer large enough to hold the whole producer output, so the consumers can be drained
    // serially without the producer ever blocking on a full buffer.
    spec.setBufferSize(1024 * 1024);

    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(getOpCtx(),
                                spec,
                                makeGroupProducerPipeline(nInputDocs, nOutputDocs),
                                exec::agg::Exchange::InputMemoryPolicy::kOwnWithoutReporting);

    // Drive both consumers through the real stage layer, each on its own opCtx.
    std::vector<ThreadInfo> threads = createNProducers(nConsumers, ex);
    for (size_t id = 0; id < nConsumers; ++id) {
        auto docSourceExchange = exec::agg::buildStage(threads[id].documentSourceExchange);
        size_t docs = 0;
        for (auto input = docSourceExchange->getNext(); input.isAdvanced();
             input = docSourceExchange->getNext()) {
            ++docs;
        }
        ASSERT_EQ(docs, nOutputDocs / nConsumers);
        docSourceExchange->dispose();

        // No consumer's CurOp ever receives the producer's memory, and no consumer opCtx holds an
        // operation tracker.
        OperationContext* consumerOpCtx = threads[id].opCtx.get();
        ASSERT_EQ(CurOp::get(consumerOpCtx)->getPeakTrackedMemoryBytes(), 0);
        ASSERT_FALSE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(consumerOpCtx));
    }

    // The producer's memory was still tracked by the Exchange-owned tracker.
    ASSERT_GT(ex->getOperationMemoryTracker_forTest()->peakTrackedMemoryBytes(),
              static_cast<int64_t>(strValLen * nInputDocs));
}

TEST_F(DocumentSourceExchangeTest, ErrorInLoadNextBatchReclaimsPublishedTracker) {
    unittest::ServerParameterGuard featureFlagController("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard curOpWriteBytes("internalQueryMaxWriteToCurOpMemoryUsageBytes",
                                                   64);

    const size_t nInputDocs = 500;
    const size_t nOutputDocs = 250;

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(1);
    spec.setBufferSize(1024);

    auto opCtx = getOpCtx();
    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(opCtx, spec, makeGroupProducerPipeline(nInputDocs, nOutputDocs));
    ASSERT_FALSE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));

    {
        FailPointEnableBlock fp("exchangeFailLoadNextBatch");
        ASSERT_THROWS_CODE(
            ex->getNext(opCtx, 0, nullptr), AssertionException, ErrorCodes::FailPointEnabled);
    }

    // The exception unwound through getNext's catch block, whose detachContext must reclaim the
    // published tracker rather than leaving it stranded on the opCtx.
    ASSERT_FALSE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));
    ASSERT(ex->getOperationMemoryTracker_forTest());

    ex->dispose(opCtx, 0);
    ASSERT_FALSE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));
}

class DocumentSourceExchangeDeathTest : public DocumentSourceExchangeTest {};

// Publishing the producer's tracker requires consumer 0's opCtx to have no operation tracker of
// its own; installing over one would free it while the consumer's stages still reference it.
DEATH_TEST_F(DocumentSourceExchangeDeathTest,
             PublishingProducerTrackerOverConsumerTrackerFails,
             "Cannot publish the exchange producer's memory tracker") {
    unittest::ServerParameterGuard featureFlagController("featureFlagQueryMemoryTracking", true);

    const auto source = getMockSource(10);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(1);
    spec.setBufferSize(1024);

    auto opCtx = getOpCtx();
    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(opCtx, spec, Pipeline::create({source}, getExpCtx()));

    // Creating any consumer-side stage tracker installs an operation tracker on consumer 0's
    // opCtx as a side effect (owned by the opCtx; the returned handle is not needed).
    OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForSBE(opCtx);
    ASSERT_TRUE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));

    ASSERT_THROWS_CODE(ex->getNext(opCtx, 0, nullptr), AssertionException, 12920100);
}

// The dispose-time flush has the same precondition as publishing during getNext: consumer 0's
// opCtx must not already hold an operation tracker, or installing the producer's tracker to
// propagate its stats would free it.
DEATH_TEST_F(DocumentSourceExchangeDeathTest,
             FlushingProducerTrackerOverConsumerTrackerFails,
             "Cannot flush the exchange producer's memory tracker") {
    unittest::ServerParameterGuard featureFlagController("featureFlagQueryMemoryTracking", true);

    const size_t nInputDocs = 10;
    const size_t nOutputDocs = 5;

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(1);
    spec.setBufferSize(1024);

    auto opCtx = getOpCtx();
    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(opCtx, spec, makeGroupProducerPipeline(nInputDocs, nOutputDocs));

    // Drain normally; each getNext publishes the tracker and reclaims it on detach.
    size_t docs = 0;
    for (auto input = ex->getNext(opCtx, 0, nullptr); input.isAdvanced();
         input = ex->getNext(opCtx, 0, nullptr)) {
        ++docs;
    }
    ASSERT_EQ(docs, nOutputDocs);

    // An operation tracker appearing on consumer 0's opCtx before dispose must trip the flush
    // guard.
    OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForSBE(opCtx);
    ASSERT_TRUE(OperationMemoryUsageTracker::hasTrackerOnOpCtx(opCtx));

    ASSERT_THROWS_CODE(ex->dispose(opCtx, 0), AssertionException, 12920101);
}

TEST_F(DocumentSourceExchangeTest, ExchangeNConsumerEarlyout) {
    const size_t nDocs = 500;
    auto source = getMockSource(500);

    const size_t nConsumers = 2;

    ASSERT_EQ(nDocs % nConsumers, 0u);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(getOpCtx(), spec, Pipeline::create({source}, getExpCtx()));

    std::vector<ThreadInfo> threads = createNProducers(nConsumers, ex);
    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    for (size_t id = 0; id < nConsumers; ++id) {
        auto docSourceExchange = exec::agg::buildStage(threads[id].documentSourceExchange);
        auto handle = _executor->scheduleWork([docSourceExchange, id, nDocs, nConsumers](
                                                  const executor::TaskExecutor::CallbackArgs& cb) {
            PseudoRandom prng(getNewSeed());

            auto input = docSourceExchange->getNext();

            size_t docs = 0;

            for (; input.isAdvanced(); input = docSourceExchange->getNext()) {
                sleepmillis(prng.nextInt32() % 20 + 1);
                ++docs;

                // The consumer 1 bails out early wihout consuming all its documents.
                if (id == 1 && docs == 100) {
                    // Pretend we have seen all docs.
                    docs = nDocs / nConsumers;

                    docSourceExchange->dispose();
                    break;
                }
            }
            ASSERT_EQ(docs, nDocs / nConsumers);
        });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);
}

TEST_F(DocumentSourceExchangeTest, DisposeIsIdempotentPerConsumer) {
    const size_t nDocs = 10;
    auto source = getMockSource(nDocs);

    const size_t nConsumers = 2;

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    auto opCtx = getOpCtx();
    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(opCtx, spec, Pipeline::create({source}, getExpCtx()));

    // Without the guard, the second call here would bump _disposeRunDown to getConsumers() and
    // call ExchangeBuffer::dispose() a second time, which invariants on !_disposed.
    ex->dispose(opCtx, 0);
    ex->dispose(opCtx, 0);
    ex->dispose(opCtx, 0);

    // The remaining consumer must still drive the rundown counter to getConsumers() and tear down
    // the inner pipeline exactly once. Without the guard, _disposeRunDown would already be at
    // getConsumers() and this call would trip invariant(_disposeRunDown < getConsumers()).
    ex->dispose(opCtx, 1);
    ex->dispose(opCtx, 1);
}

// When a consumer throws while loading the next batch, it latches the error in
// _errorInLoadNextBatch and becomes the _loadingThreadId. Disposal then takes a different path:
// only the loading thread tears down the inner pipeline, and the per-consumer rundown counter no
// longer gates that teardown. Verify disposal is correct (and still double-dispose safe) in that
// error state.
TEST_F(DocumentSourceExchangeTest, DisposeAfterErrorInLoadNextBatch) {
    const size_t nDocs = 10;
    auto source = getMockSource(nDocs);

    const size_t nConsumers = 2;

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    auto opCtx = getOpCtx();
    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(opCtx, spec, Pipeline::create({source}, getExpCtx()));

    {
        // Force the first load to throw. The throwing consumer (0) becomes the loading thread and
        // latches the error into _errorInLoadNextBatch.
        FailPointEnableBlock fp("exchangeFailLoadNextBatch");
        ASSERT_THROWS_CODE(
            ex->getNext(opCtx, 0, nullptr), AssertionException, ErrorCodes::FailPointEnabled);

        // Now that the error is latched, any other consumer observes it as a passthrough error
        // rather than attempting to load again.
        ASSERT_THROWS_CODE(
            ex->getNext(opCtx, 1, nullptr), AssertionException, ErrorCodes::ExchangePassthrough);
    }

    // Disposal in the error state must succeed. The non-loading consumer (1) does not tear down the
    // inner pipeline; the loading consumer (0) does. Neither over-runs the rundown counter.
    ex->dispose(opCtx, 1);
    ex->dispose(opCtx, 0);

    // The double-dispose guard must also hold in the error path: both consumers have now disposed
    // once, so _disposeRunDown == getConsumers(). Without the guard, this repeat would trip
    // invariant(_disposeRunDown < getConsumers()).
    ex->dispose(opCtx, 0);
    ex->dispose(opCtx, 1);
}

TEST_F(DocumentSourceExchangeTest, BroadcastExchangeNConsumer) {
    const size_t nDocs = 500;
    auto source = getMockSource(nDocs);

    const size_t nConsumers = 5;

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kBroadcast);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(getOpCtx(), spec, Pipeline::create({source}, getExpCtx()));

    std::vector<ThreadInfo> threads = createNProducers(nConsumers, ex);
    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    for (size_t id = 0; id < nConsumers; ++id) {
        auto docSourceExchange = exec::agg::buildStage(threads[id].documentSourceExchange);
        auto handle = _executor->scheduleWork(
            [docSourceExchange, id, nDocs](const executor::TaskExecutor::CallbackArgs& cb) {
                size_t docs = 0;
                for (auto input = docSourceExchange->getNext(); input.isAdvanced();
                     input = docSourceExchange->getNext()) {
                    ++docs;
                }
                ASSERT_EQ(docs, nDocs);
            });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);
}

TEST_F(DocumentSourceExchangeTest, RangeExchangeNConsumer) {
    const size_t nDocs = 500;
    auto source = getMockSource(nDocs);

    const std::vector<BSONObj> boundaries = {BSON("a" << MINKEY),
                                             BSON("a" << 100),
                                             BSON("a" << 200),
                                             BSON("a" << 300),
                                             BSON("a" << 400),
                                             BSON("a" << MAXKEY)};

    const size_t nConsumers = boundaries.size() - 1;

    ASSERT(nDocs % nConsumers == 0);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kKeyRange);
    spec.setKey(BSON("a" << 1));
    spec.setBoundaries(boundaries);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<exec::agg::Exchange> ex = new exec::agg::Exchange(
        getOpCtx(), std::move(spec), Pipeline::create({source}, getExpCtx()));

    std::vector<ThreadInfo> threads = createNProducers(nConsumers, ex);
    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    for (size_t id = 0; id < nConsumers; ++id) {
        auto docSourceExchange = exec::agg::buildStage(threads[id].documentSourceExchange);
        auto handle = _executor->scheduleWork([docSourceExchange, id, nDocs, nConsumers](
                                                  const executor::TaskExecutor::CallbackArgs& cb) {
            size_t docs = 0;
            for (auto input = docSourceExchange->getNext(); input.isAdvanced();
                 input = docSourceExchange->getNext()) {
                size_t value = input.getDocument()["a"].getInt();

                ASSERT(value >= id * 100);
                ASSERT(value < (id + 1) * 100);

                ++docs;
            }

            ASSERT_EQ(docs, nDocs / nConsumers);
        });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);
}

TEST_F(DocumentSourceExchangeTest, RangeShardingExchangeNConsumer) {
    const size_t nDocs = 500;
    auto source = getMockSource(nDocs);

    const std::vector<BSONObj> boundaries = {
        BSON("a" << MINKEY),
        BSON("a" << 50),
        BSON("a" << 100),
        BSON("a" << 150),
        BSON("a" << 200),
        BSON("a" << 250),
        BSON("a" << 300),
        BSON("a" << 350),
        BSON("a" << 400),
        BSON("a" << 450),
        BSON("a" << MAXKEY),
    };
    std::vector<int> consumerIds({0, 0, 1, 1, 2, 2, 3, 3, 4, 4});

    const size_t nConsumers = consumerIds.size() / 2;

    ASSERT(nDocs % nConsumers == 0);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kKeyRange);
    spec.setKey(BSON("a" << 1));
    spec.setBoundaries(boundaries);
    spec.setConsumerIds(consumerIds);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<exec::agg::Exchange> ex = new exec::agg::Exchange(
        getOpCtx(), std::move(spec), Pipeline::create({source}, getExpCtx()));

    std::vector<ThreadInfo> threads = createNProducers(nConsumers, ex);
    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    for (size_t id = 0; id < nConsumers; ++id) {
        auto docSourceExchange = exec::agg::buildStage(threads[id].documentSourceExchange);
        auto handle = _executor->scheduleWork([docSourceExchange, id, nDocs, nConsumers](
                                                  const executor::TaskExecutor::CallbackArgs& cb) {
            size_t docs = 0;
            for (auto input = docSourceExchange->getNext(); input.isAdvanced();
                 input = docSourceExchange->getNext()) {
                size_t value = input.getDocument()["a"].getInt();

                ASSERT(value >= id * 100);
                ASSERT(value < (id + 1) * 100);

                ++docs;
            }

            ASSERT_EQ(docs, nDocs / nConsumers);
        });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);
}

TEST_F(DocumentSourceExchangeTest, RangeRandomExchangeNConsumer) {
    const size_t nDocs = 500;
    auto source = getRandomMockSource(nDocs, getNewSeed());

    const std::vector<BSONObj> boundaries = {BSON("a" << MINKEY),
                                             BSON("a" << 100),
                                             BSON("a" << 200),
                                             BSON("a" << 300),
                                             BSON("a" << 400),
                                             BSON("a" << MAXKEY)};

    const size_t nConsumers = boundaries.size() - 1;

    ASSERT(nDocs % nConsumers == 0);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kKeyRange);
    spec.setKey(BSON("a" << 1));
    spec.setBoundaries(boundaries);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<exec::agg::Exchange> ex = new exec::agg::Exchange(
        getOpCtx(), std::move(spec), Pipeline::create({source}, getExpCtx()));

    std::vector<ThreadInfo> threads = createNProducers(nConsumers, ex);
    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    Atomic<size_t> processedDocs{0};

    for (size_t id = 0; id < nConsumers; ++id) {
        auto docSourceExchange = exec::agg::buildStage(threads[id].documentSourceExchange);
        auto handle = _executor->scheduleWork([docSourceExchange, id, &processedDocs](
                                                  const executor::TaskExecutor::CallbackArgs& cb) {
            PseudoRandom prng(getNewSeed());

            auto input = docSourceExchange->getNext();

            size_t docs = 0;
            for (; input.isAdvanced(); input = docSourceExchange->getNext()) {
                size_t value = input.getDocument()["a"].getInt();

                ASSERT(value >= id * 100);
                ASSERT(value < (id + 1) * 100);

                ++docs;

                // This helps randomizing thread scheduling forcing different threads to load
                // buffers. The sleep API is inherently imprecise so we cannot guarantee 100%
                // reproducibility.
                sleepmillis(prng.nextInt32() % 50 + 1);
            }
            processedDocs.fetchAndAdd(docs);
        });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);

    ASSERT_EQ(nDocs, processedDocs.load());
}

TEST_F(DocumentSourceExchangeTest, RandomExchangeNConsumerResourceYielding) {
    const size_t nDocs = 500;
    auto source = getRandomMockSource(nDocs, getNewSeed());

    const std::vector<BSONObj> boundaries = {
        BSON("a" << MINKEY), BSON("a" << 500), BSON("a" << MAXKEY)};

    const size_t nConsumers = boundaries.size() - 1;

    ASSERT(nDocs % nConsumers == 0);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kKeyRange);
    spec.setKey(BSON("a" << 1));
    spec.setBoundaries(boundaries);
    spec.setConsumers(nConsumers);

    // Tiny buffer so if there are deadlocks possible they reproduce more often.
    spec.setBufferSize(64);

    // An "artifical" mutex that's not actually necessary for thread safety. We enforce that each
    // thread holds this while it calls getNext(). This is to simulate the case where a thread may
    // hold some "real" resources which need to be yielded while waiting, such as the Session, or
    // the locks held in a transaction.
    std::mutex artificalGlobalMutex;

    boost::intrusive_ptr<exec::agg::Exchange> ex = new exec::agg::Exchange(
        getOpCtx(), std::move(spec), Pipeline::create({source}, getExpCtx()));
    std::vector<ThreadInfo> threads;

    for (size_t idx = 0; idx < nConsumers; ++idx) {
        ServiceContext::UniqueClient client =
            getServiceContext()->getService()->makeClient("exchange client");
        ServiceContext::UniqueOperationContext opCtxOwned =
            getServiceContext()->makeOperationContext(client.get());
        OperationContext* opCtx = opCtxOwned.get();
        auto yielder = std::make_unique<MutexYielder>(&artificalGlobalMutex);
        auto yielderRaw = yielder.get();
        threads.push_back(ThreadInfo{
            std::move(client),
            std::move(opCtxOwned),
            new DocumentSourceExchange(ExpressionContextBuilder{}.opCtx(opCtx).ns(kTestNss).build(),
                                       ex,
                                       idx,
                                       std::move(yielder)),
            yielderRaw});
    }

    std::vector<executor::TaskExecutor::CallbackHandle> handles;

    Atomic<size_t> processedDocs{0};

    for (size_t id = 0; id < nConsumers; ++id) {
        ThreadInfo* threadInfo = &threads[id];
        auto handle = _executor->scheduleWork(
            [threadInfo, &processedDocs](const executor::TaskExecutor::CallbackArgs& cb) {
                auto docSourceExchange = exec::agg::buildStage(threadInfo->documentSourceExchange);
                const auto getNext = [docSourceExchange, threadInfo]() {
                    // Will acquire 'artificalGlobalMutex'. Within getNext() it will be released and
                    // reacquired by the MutexYielder if the Exchange has to block.
                    threadInfo->yielder->getLock().lock();
                    auto res = docSourceExchange->getNext();
                    threadInfo->yielder->getLock().unlock();
                    return res;
                };

                for (auto input = getNext(); input.isAdvanced(); input = getNext()) {
                    // This helps randomizing thread scheduling forcing different threads to load
                    // buffers. The sleep API is inherently imprecise so we cannot guarantee 100%
                    // reproducibility.
                    PseudoRandom prng(getNewSeed());
                    sleepmillis(prng.nextInt32() % 50 + 1);
                    processedDocs.fetchAndAdd(1);
                }
            });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);

    ASSERT_EQ(nDocs, processedDocs.load());
}

TEST_F(DocumentSourceExchangeTest, RangeRandomHashExchangeNConsumer) {
    const size_t nDocs = 500;
    auto source = getRandomMockSource(nDocs, getNewSeed());

    const std::vector<BSONObj> boundaries = {
        BSON("a" << MINKEY),
        BSON("a" << BSONElementHasher::hash64(BSON("" << 0).firstElement(),
                                              BSONElementHasher::DEFAULT_HASH_SEED)),
        BSON("a" << MAXKEY)};

    const size_t nConsumers = boundaries.size() - 1;

    ASSERT(nDocs % nConsumers == 0);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kKeyRange);
    spec.setKey(BSON("a" << "hashed"));
    spec.setBoundaries(boundaries);
    spec.setConsumers(nConsumers);
    spec.setBufferSize(1024);

    boost::intrusive_ptr<exec::agg::Exchange> ex = new exec::agg::Exchange(
        getOpCtx(), std::move(spec), Pipeline::create({source}, getExpCtx()));

    std::vector<ThreadInfo> threads = createNProducers(nConsumers, ex);
    std::vector<executor::TaskExecutor::CallbackHandle> handles;
    Atomic<size_t> processedDocs{0};

    for (size_t id = 0; id < nConsumers; ++id) {
        auto docSourceExchange = exec::agg::buildStage(threads[id].documentSourceExchange);
        auto handle = _executor->scheduleWork([docSourceExchange, id, &processedDocs](
                                                  const executor::TaskExecutor::CallbackArgs& cb) {
            PseudoRandom prng(getNewSeed());

            auto input = docSourceExchange->getNext();

            size_t docs = 0;
            for (; input.isAdvanced(); input = docSourceExchange->getNext()) {
                ++docs;

                // This helps randomizing thread scheduling forcing different threads to load
                // buffers. The sleep API is inherently imprecise so we cannot guarantee 100%
                // reproducibility.
                sleepmillis(prng.nextInt32() % 50 + 1);
            }
            processedDocs.fetchAndAdd(docs);
        });

        handles.emplace_back(std::move(handle.getValue()));
    }

    for (auto& h : handles)
        _executor->wait(h);

    ASSERT_EQ(nDocs, processedDocs.load());
}

TEST_F(DocumentSourceExchangeTest, RejectNoConsumers) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "broadcast"
                                 << "consumers" << 0);
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        50901);
}

TEST_F(DocumentSourceExchangeTest, RejectInvalidKey) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "broadcast"
                                 << "consumers" << 1 << "key" << BSON("a" << 2));
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        50896);
}

TEST_F(DocumentSourceExchangeTest, RejectInvalidKeyHashExpected) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "broadcast"
                                 << "consumers" << 1 << "key" << BSON("a" << "nothash"));
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        50895);
}

TEST_F(DocumentSourceExchangeTest, RejectInvalidKeyWrongType) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "broadcast"
                                 << "consumers" << 1 << "key" << BSON("a" << true));
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        50897);
}

TEST_F(DocumentSourceExchangeTest, RejectInvalidKeyEmpty) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "broadcast"
                                 << "consumers" << 1 << "key" << BSON("" << 1));
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        40352);
}

TEST_F(DocumentSourceExchangeTest, RejectInvalidBoundaries) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "keyRange"
                                 << "consumers" << 1 << "key" << BSON("a" << 1) << "boundaries"
                                 << BSON_ARRAY(BSON("a" << MAXKEY) << BSON("a" << MINKEY))
                                 << "consumerIds" << BSON_ARRAY(0));
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        50893);
}

TEST_F(DocumentSourceExchangeTest, RejectInvalidBoundariesMissingMin) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "keyRange"
                                 << "consumers" << 1 << "key" << BSON("a" << 1) << "boundaries"
                                 << BSON_ARRAY(BSON("a" << 0) << BSON("a" << MAXKEY))
                                 << "consumerIds" << BSON_ARRAY(0));
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        50958);
}

TEST_F(DocumentSourceExchangeTest, RejectInvalidBoundariesMissingMax) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "keyRange"
                                 << "consumers" << 1 << "key" << BSON("a" << 1) << "boundaries"
                                 << BSON_ARRAY(BSON("a" << MINKEY) << BSON("a" << 0))
                                 << "consumerIds" << BSON_ARRAY(0));
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        50959);
}

TEST_F(DocumentSourceExchangeTest, RejectInvalidBoundariesAndConsumerIds) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "keyRange"
                                 << "consumers" << 2 << "key" << BSON("a" << 1) << "boundaries"
                                 << BSON_ARRAY(BSON("a" << MINKEY) << BSON("a" << MAXKEY))
                                 << "consumerIds" << BSON_ARRAY(0 << 1));
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        50900);
}

TEST_F(DocumentSourceExchangeTest, RejectInvalidPolicyBoundaries) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "roundrobin"
                                 << "consumers" << 1 << "key" << BSON("a" << 1) << "boundaries"
                                 << BSON_ARRAY(BSON("a" << MINKEY) << BSON("a" << MAXKEY))
                                 << "consumerIds" << BSON_ARRAY(0));
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        50899);
}

TEST_F(DocumentSourceExchangeTest, RejectInvalidConsumerIds) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "keyRange"
                                 << "consumers" << 1 << "key" << BSON("a" << 1) << "boundaries"
                                 << BSON_ARRAY(BSON("a" << MINKEY) << BSON("a" << MAXKEY))
                                 << "consumerIds" << BSON_ARRAY(1));
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        50894);
}

TEST_F(DocumentSourceExchangeTest, RejectInvalidMissingKeys) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    BSONObj spec = BSON("policy" << "keyRange"
                                 << "consumers" << 1 << "boundaries"
                                 << BSON_ARRAY(BSON("a" << MINKEY) << BSON("a" << MAXKEY))
                                 << "consumerIds" << BSON_ARRAY(0));
    ASSERT_THROWS_CODE(
        exec::agg::Exchange(getOpCtx(), parseSpec(spec), Pipeline::create({source}, getExpCtx())),
        AssertionException,
        50967);
}

// Validates that when a document field value is exactly MaxKey, the exchange correctly
// routes the document to the last consumer bucket.
TEST_F(DocumentSourceExchangeTest, KeyRangeRoutesMaxKeyToLastBucket) {
    // Source emits one document whose 'a' field is MaxKey.
    auto source = DocumentSourceMock::createForTest(Document{{"a", Value(MAXKEY)}}, getExpCtx());

    const std::vector<BSONObj> boundaries = {BSON("a" << MINKEY), BSON("a" << MAXKEY)};

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kKeyRange);
    spec.setKey(BSON("a" << 1));
    spec.setBoundaries(boundaries);
    spec.setConsumers(1);
    spec.setBufferSize(1024);

    auto opCtx = getOpCtx();
    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(opCtx, std::move(spec), Pipeline::create({source}, getExpCtx()));

    // Validate that the exchange correctly processes the document.
    size_t docs = 0;
    for (auto input = ex->getNext(opCtx, 0, nullptr); input.isAdvanced();
         input = ex->getNext(opCtx, 0, nullptr)) {
        ++docs;
    }
    ASSERT_EQ(1u, docs);
}

TEST_F(DocumentSourceExchangeTest, QueryShape) {
    const size_t nDocs = 500;

    auto source = getMockSource(nDocs);

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kRoundRobin);
    spec.setConsumers(1);
    spec.setBufferSize(1024);
    boost::intrusive_ptr<exec::agg::Exchange> ex =
        new exec::agg::Exchange(getOpCtx(), spec, Pipeline::create({source}, getExpCtx()));
    boost::intrusive_ptr<DocumentSourceExchange> stage =
        new DocumentSourceExchange(getExpCtx(), ex, 0, nullptr);

    ASSERT_BSONOBJ_EQ_AUTO(  //
        R"({
            "$_internalExchange": {
                "policy": "roundrobin",
                "consumers": "?number",
                "orderPreserving": false,
                "bufferSize": "?number",
                "key": "?object"
            }
        })",
        redact(*stage));
}

}  // namespace mongo
