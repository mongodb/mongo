// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/repl_compression_pipeline.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/server_parameter.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/message_compressor_snappy.h"
#include "mongo/transport/repl_compression_pipeline_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport {

class ReplCompressionThreadPoolTest : public unittest::Test {
protected:
    static std::unique_ptr<ReplCompressionThreadPool> makePool() {
        return std::unique_ptr<ReplCompressionThreadPool>(new ReplCompressionThreadPool());
    }
};

namespace {

// Unit tests don't set up compressors like mongod does, so register snappy if needed.
MessageCompressorId snappyId() {
    auto& registry = MessageCompressorRegistry::get();
    if (!registry.getCompressor("snappy")) {
        registry.setSupportedCompressors({"snappy"});
        registry.registerImplementation(std::make_unique<SnappyMessageCompressor>());
        uassertStatusOK(registry.finalizeSupportedCompressors());
    }
    auto* c = registry.getCompressor("snappy");
    ASSERT(c);
    return c->getId();
}

// Small OP_MSG tagged with `marker`.
Message makeMsg(int marker) {
    OpMsgBuilder builder;
    builder.setBody(BSON("marker" << marker << "pad" << std::string(256, 'x')));
    auto m = builder.finish();
    m.header().setId(1000 + marker);
    m.header().setResponseToMsgId(2000 + marker);
    return m;
}

// Sets runtime pipeline parameters for one test. The pool size is not settable here: the shared
// pool is a process-wide singleton started once in setUp() with the default size, so tests must
// not depend on a particular number of workers.
class ParamGuard {
public:
    ParamGuard(bool enabled, int depth, long long maxPendingBytes = 0) {
        _enabled = gReplPipelineCompressionEnabled.load();
        _depth = gReplPipelineCompressionQueueDepth.load();
        _bytes = gReplPipelineCompressionMaxPendingBytes.load();
        gReplPipelineCompressionEnabled.store(enabled);
        gReplPipelineCompressionQueueDepth.store(depth);
        gReplPipelineCompressionMaxPendingBytes.store(maxPendingBytes);
    }
    ~ParamGuard() {
        gReplPipelineCompressionEnabled.store(_enabled);
        gReplPipelineCompressionQueueDepth.store(_depth);
        gReplPipelineCompressionMaxPendingBytes.store(_bytes);
    }

private:
    bool _enabled;
    int _depth;
    long long _bytes;
};

// Returns the marker of a compressed or passthrough message.
int markerOf(const Message& m) {
    Message plain = m;
    if (m.operation() == dbCompressed) {
        MessageCompressorManager mgr;
        plain = uassertStatusOK(mgr.decompressMessage(m));
    }
    auto opMsg = OpMsg::parse(plain);
    return opMsg.body["marker"].numberInt();
}

// Cancelled tasks release their reservation asynchronously, so wait for leftovers from earlier
// tests before checking the budget.
bool waitForBudgetToDrain(Milliseconds timeout = Seconds(30)) {
    const auto deadline = Date_t::now() + timeout;
    while (ReplyCompressionPipeline::globalPendingBytes() != 0) {
        if (Date_t::now() > deadline) {
            return false;
        }
        sleepFor(Milliseconds(1));
    }
    return true;
}

class ReplCompressionPipelineTest : public unittest::Test {
protected:
    void setUp() override {
        ReplCompressionThreadPool::get().ensureStarted();
        ASSERT_TRUE(waitForBudgetToDrain());
    }
    void tearDown() override {
        ASSERT_TRUE(waitForBudgetToDrain());
    }
};

// Read a Counter64/gauge published under metrics.network.replCompressionPipeline.<leaf>.
long long pipelineMetric(std::string_view leaf) {
    BSONObjBuilder bob;
    globalMetricTreeSet()[ClusterRole::None].appendTo(bob);
    auto obj = bob.obj();
    auto elem = obj["metrics"]["network"]["replCompressionPipeline"][leaf];
    return elem.eoo() ? -1 : elem.safeNumberLong();
}


TEST_F(ReplCompressionPipelineTest, SingleSubmitRoundTrips) {
    ParamGuard guard(true, 4);
    auto pipeline = ReplyCompressionPipeline::make();

    Message in = makeMsg(0);
    ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
    ASSERT_TRUE(in.empty());

    Message out;
    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_EQ(markerOf(out), 0);
    ASSERT_FALSE(pipeline->hasPending());
}

// Workers may finish in any order; batches still come out in submit order.
TEST_F(ReplCompressionPipelineTest, PreservesOrderAcrossManyBatches) {
    ParamGuard guard(true, 64);
    auto pipeline = ReplyCompressionPipeline::make();

    constexpr int kN = 50;
    for (int i = 0; i < kN; ++i) {
        Message in = makeMsg(i);
        ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
    }

    std::vector<int> order;
    for (int i = 0; i < kN; ++i) {
        Message out;
        ASSERT_TRUE(pipeline->takeNextReady(&out));
        order.push_back(markerOf(out));
    }
    for (int i = 0; i < kN; ++i) {
        ASSERT_EQ(order[i], i);
    }
    ASSERT_FALSE(pipeline->hasPending());
}


TEST_F(ReplCompressionPipelineTest, BackpressureRejectsWhenFull) {
    ParamGuard guard(true, 2);
    auto pipeline = ReplyCompressionPipeline::make();

    Message a = makeMsg(0);
    Message b = makeMsg(1);
    ASSERT_TRUE(pipeline->trySubmit(a, snappyId()));
    ASSERT_TRUE(pipeline->trySubmit(b, snappyId()));

    // Window is full.
    Message c = makeMsg(2);
    ASSERT_FALSE(pipeline->trySubmit(c, snappyId()));
    ASSERT_FALSE(c.empty());

    Message out;
    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_EQ(markerOf(out), 0);

    ASSERT_TRUE(pipeline->trySubmit(c, snappyId()));
    ASSERT_TRUE(c.empty());
}

TEST_F(ReplCompressionPipelineTest, DisabledAtRuntimeDeclines) {
    ParamGuard guard(false /* disabled */, 4);
    auto pipeline = ReplyCompressionPipeline::make();

    Message in = makeMsg(0);
    ASSERT_EQ(ReplyCompressionPipeline::effectiveQueueDepth(), 0u);
    ASSERT_FALSE(pipeline->trySubmit(in, snappyId()));
    ASSERT_FALSE(in.empty());
}

// The byte parameters go through the registered validators, which setParameter and startup
// options also use. This checks the bounds only, not the 4096 floor applied when MinResponseBytes
// is read. MinResponseBytes accepts values below that floor, and MaxPendingBytes has no upper bound
// other than the range of long long.
TEST(ReplCompressionPipelineParamsTest, ByteParameterBounds) {
    const auto checkBounds = [](std::string_view name,
                                const std::vector<long long>& accepted,
                                const std::vector<long long>& rejected) {
        auto* sp = ServerParameterSet::getNodeParameterSet()->get(name);
        ASSERT(sp);
        for (long long v : accepted) {
            ASSERT_OK(sp->validate(BSON("" << v).firstElement(), boost::none)) << name << "=" << v;
        }
        for (long long v : rejected) {
            ASSERT_NOT_OK(sp->validate(BSON("" << v).firstElement(), boost::none))
                << name << "=" << v;
        }
    };
    const long long kHuge = std::numeric_limits<long long>::max();
    checkBounds("internalReplPipelineCompressionMinResponseBytes", {0, 1, 4095, 4096, kHuge}, {-1});
    checkBounds("internalReplPipelineCompressionMaxPendingBytes",
                {0, 1, 17179869185LL, kHuge},
                {-1});
}

TEST_F(ReplCompressionPipelineTest, GlobalPendingBytesCapDeclinesNewWork) {
    ParamGuard guard(true, 4, 1 /* maxPendingBytes */);
    auto pipeline = ReplyCompressionPipeline::make();

    Message in = makeMsg(0);
    // Allowed: the budget starts below the cap.
    ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));

    // Now over the cap.
    ASSERT_EQ(ReplyCompressionPipeline::effectiveQueueDepth(), 0u);
    Message declined = makeMsg(1);
    ASSERT_FALSE(pipeline->trySubmit(declined, snappyId()));
    ASSERT_FALSE(declined.empty());

    Message out;
    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_EQ(markerOf(out), 0);

    ASSERT_TRUE(ReplyCompressionPipeline::effectiveQueueDepth() > 0u);
    ASSERT_TRUE(pipeline->trySubmit(declined, snappyId()));
}

// Once a batch is compressed it holds only the (smaller) result in the budget, so new work is
// admitted again without draining.
TEST_F(ReplCompressionPipelineTest, RawReleasedAfterCompressionFreesGlobalBudget) {
    OpMsgBuilder builder;
    builder.setBody(BSON("marker" << 0 << "pad" << std::string(64 * 1024, 'a')));
    auto big = builder.finish();
    big.header().setId(1);
    big.header().setResponseToMsgId(2);
    const auto rawSize = static_cast<long long>(big.size());

    // Cap sits between compressedSize and rawSize: while the batch is uncompressed (holding
    // rawSize) the cap is exceeded and new work is declined; once compressed (holding only the
    // small compressedSize) the cap is back under budget and new work is admitted.
    ParamGuard guard(true, 4, rawSize / 2 /* maxPendingBytes */);
    auto pipeline = ReplyCompressionPipeline::make();

    Message in = big;
    ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));

    // Retry without draining until the first batch has been compressed.
    Message again = makeMsg(1);
    bool submittedSecond = false;
    for (int i = 0; i < 100000 && !submittedSecond; ++i) {
        submittedSecond = pipeline->trySubmit(again, snappyId());
        if (!submittedSecond) {
            ASSERT_FALSE(again.empty());
            std::this_thread::yield();
        }
    }
    ASSERT_TRUE(submittedSecond);
    ASSERT_TRUE(again.empty());

    Message out;
    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_EQ(markerOf(out), 0);
    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_EQ(markerOf(out), 1);
    ASSERT_FALSE(pipeline->hasPending());
}

// A raw reply is reserved by its buffer's capacity, not its wire size: getMore replies can keep a
// buffer much larger than the reply. The reservation is released in full once the batch is taken.
TEST_F(ReplCompressionPipelineTest, RawBudgetUsesBufferCapacity) {
    ParamGuard guard(true, 4);
    auto pipeline = ReplyCompressionPipeline::make();

    constexpr size_t kExtraCapacity = 1024 * 1024;
    Message in = makeMsg(0);
    const auto wireSize = static_cast<size_t>(in.size());
    in.realloc(wireSize + kExtraCapacity);
    const auto capacity = in.capacity();
    ASSERT_GTE(capacity, wireSize + kExtraCapacity);

    const long long rawBytes0 = pipelineMetric("rawBytes");
    {
        // Keep the batch uncompressed so its raw reservation is observable.
        FailPointEnableBlock hang("replPipelineHangBeforeCompression");
        ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
        ASSERT_EQ(ReplyCompressionPipeline::globalPendingBytes(), static_cast<long long>(capacity));
        // The metric still reports the wire size.
        ASSERT_EQ(pipelineMetric("rawBytes"), rawBytes0 + static_cast<long long>(wireSize));
    }

    Message out;
    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_EQ(markerOf(out), 0);
    ASSERT_EQ(ReplyCompressionPipeline::globalPendingBytes(), 0);
}

TEST_F(ReplCompressionPipelineTest, TryTakeIsNonBlocking) {
    ParamGuard guard(true, 8);
    auto pipeline = ReplyCompressionPipeline::make();

    Message in = makeMsg(7);
    ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));

    Message out;
    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_EQ(markerOf(out), 7);

    // Nothing pending; must return right away.
    Message out2;
    ASSERT_FALSE(pipeline->tryTakeNextReady(&out2));
}

TEST_F(ReplCompressionPipelineTest, ShutdownWakesSenderAndDropsPending) {
    ParamGuard guard(true, 8);
    auto pipeline = ReplyCompressionPipeline::make();

    // Some of these may already be compressed when we shut down.
    for (int i = 0; i < 4; ++i) {
        Message in = makeMsg(i);
        ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
    }
    pipeline->shutdown();

    Message out;
    ASSERT_FALSE(pipeline->takeNextReady(&out));
    ASSERT_TRUE(pipeline->isShutdown());
}

TEST_F(ReplCompressionPipelineTest, SubmitAfterShutdownDeclines) {
    ParamGuard guard(true, 8);
    auto pipeline = ReplyCompressionPipeline::make();
    pipeline->shutdown();

    Message in = makeMsg(0);
    ASSERT_FALSE(pipeline->trySubmit(in, snappyId()));
    ASSERT_FALSE(in.empty());
}

TEST_F(ReplCompressionPipelineTest, TryTakeDrainsInOrder) {
    ParamGuard guard(true, 16);
    auto pipeline = ReplyCompressionPipeline::make();

    constexpr int kN = 10;
    for (int i = 0; i < kN; ++i) {
        Message in = makeMsg(i);
        ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
    }

    std::vector<int> order;
    while (static_cast<int>(order.size()) < kN) {
        Message out;
        if (pipeline->tryTakeNextReady(&out)) {
            order.push_back(markerOf(out));
        }
    }
    for (int i = 0; i < kN; ++i) {
        ASSERT_EQ(order[i], i);
    }
    ASSERT_FALSE(pipeline->hasPending());
}

// Submitting and taking a batch advances the submitted/taken counters and moves rawBytes/
// compressedBytes, and leaves the in-flight gauge (rawQueueDepth) back at its starting value.
// batchesSent only moves when the caller reports a completed network write.
TEST_F(ReplCompressionPipelineTest, MetricsTrackSubmitAndSend) {
    ParamGuard guard(true, 8);
    auto pipeline = ReplyCompressionPipeline::make();

    const long long submitted0 = pipelineMetric("batchesSubmitted");
    const long long taken0 = pipelineMetric("batchesTaken");
    const long long sent0 = pipelineMetric("batchesSent");
    const long long rawBytes0 = pipelineMetric("rawBytes");
    const long long compressedBytes0 = pipelineMetric("compressedBytes");
    const long long depth0 = pipelineMetric("rawQueueDepth");
    const long long latCount0 = pipelineMetric("compressLatencyCount");

    Message in = makeMsg(0);
    const auto rawSize = static_cast<long long>(in.size());
    ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));

    ASSERT_EQ(pipelineMetric("batchesSubmitted"), submitted0 + 1);
    ASSERT_EQ(pipelineMetric("rawBytes"), rawBytes0 + rawSize);
    ASSERT_EQ(pipelineMetric("rawQueueDepth"), depth0 + 1);

    Message out;
    ASSERT_TRUE(pipeline->takeNextReady(&out));

    // After the take: taken counter +1, gauge back to baseline, compression latency recorded,
    // compressed bytes advanced by the (nonzero) compressed size. Nothing was written yet.
    ASSERT_EQ(pipelineMetric("batchesTaken"), taken0 + 1);
    ASSERT_EQ(pipelineMetric("batchesSent"), sent0);
    ASSERT_EQ(pipelineMetric("rawQueueDepth"), depth0);
    ASSERT_EQ(pipelineMetric("compressLatencyCount"), latCount0 + 1);
    ASSERT_GT(pipelineMetric("compressedBytes"), compressedBytes0);
    ASSERT_FALSE(pipeline->hasPending());

    ReplyCompressionPipeline::recordBatchSent();
    ASSERT_EQ(pipelineMetric("batchesSent"), sent0 + 1);
}

// A batch whose task can't be scheduled still completes, in seq order, with a failure, and its
// reservation is released. Later batches are not reordered ahead of it.
TEST_F(ReplCompressionPipelineTest, ScheduleFailurePublishesFailureAndReleasesBudget) {
    ParamGuard guard(true, 4);
    auto pipeline = ReplyCompressionPipeline::make();
    const auto failures = pipelineMetric("compressFailures");
    const auto depth = pipelineMetric("rawQueueDepth");
    {
        FailPointEnableBlock failPoint("replPipelineScheduleThrows");
        Message in = makeMsg(0);
        ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
        ASSERT_TRUE(in.empty());
    }
    Message next = makeMsg(1);
    ASSERT_TRUE(pipeline->trySubmit(next, snappyId()));

    Message out;
    ASSERT_THROWS_CODE(pipeline->takeNextReady(&out), DBException, ErrorCodes::InternalError);
    ASSERT_EQ(pipelineMetric("compressFailures"), failures + 1);

    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_EQ(markerOf(out), 1);
    ASSERT_FALSE(pipeline->hasPending());
    ASSERT_EQ(pipelineMetric("rawQueueDepth"), depth);
    ASSERT_EQ(ReplyCompressionPipeline::globalPendingBytes(), 0);
}

// The byte cap is checked and reserved atomically: once a batch crosses it, no other submit is
// admitted until the budget falls back below it, even from other sessions.
TEST_F(ReplCompressionPipelineTest, GlobalPendingBytesCapIsAtomicAcrossPipelines) {
    ParamGuard guard(true, 64, 1 /* maxPendingBytes */);
    // Keep every admitted batch at its raw reservation, so none of them can make room.
    FailPointEnableBlock hang("replPipelineHangBeforeCompression");

    constexpr int kThreads = 8;
    std::vector<std::shared_ptr<ReplyCompressionPipeline>> pipelines;
    for (int i = 0; i < kThreads; ++i) {
        pipelines.push_back(ReplyCompressionPipeline::make());
    }
    std::atomic<bool> start{false};  // NOLINT
    std::atomic<int> admitted{0};    // NOLINT
    std::vector<stdx::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            while (!start.load()) {
                std::this_thread::yield();
            }
            Message in = makeMsg(i);
            if (pipelines[i]->trySubmit(in, snappyId())) {
                admitted.fetch_add(1);
            }
        });
    }
    start.store(true);
    for (auto& t : threads) {
        t.join();
    }
    ASSERT_EQ(admitted.load(), 1);

    // Paused tasks see the shutdown when released and give back their reservation (checked by
    // tearDown()).
    for (auto& p : pipelines) {
        p->shutdown();
    }
}

TEST_F(ReplCompressionPipelineTest, ShutdownReleasesGaugeForDroppedBatches) {
    ParamGuard guard(true, 8);
    const long long depth0 = pipelineMetric("rawQueueDepth");
    {
        auto pipeline = ReplyCompressionPipeline::make();
        for (int i = 0; i < 4; ++i) {
            Message in = makeMsg(i);
            ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
        }
        ASSERT_GT(pipelineMetric("rawQueueDepth"), depth0);
        pipeline->shutdown();
    }
    ASSERT_EQ(pipelineMetric("rawQueueDepth"), depth0);
}

// The queued result is copied into an exact-size buffer instead of keeping the worst-case one.
TEST_F(ReplCompressionPipelineTest, CompressedResultIsCompactedToExactCapacity) {
    OpMsgBuilder builder;
    builder.setBody(BSON("marker" << 0 << "pad" << std::string(1024 * 1024, 'a')));
    auto big = builder.finish();
    big.header().setId(1);
    big.header().setResponseToMsgId(2);
    const auto rawSize = static_cast<size_t>(big.size());

    ParamGuard guard(true, 4);
    auto pipeline = ReplyCompressionPipeline::make();
    Message in = big;
    big = Message{};
    ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));

    Message out;
    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_EQ(out.operation(), dbCompressed);
    ASSERT_LT(static_cast<size_t>(out.size()), rawSize / 4);
    ASSERT_EQ(out.capacity(), static_cast<size_t>(out.size()));
    ASSERT_EQ(markerOf(out), 0);
    ASSERT_FALSE(pipeline->hasPending());
}

TEST_F(ReplCompressionPipelineTest, ShutdownUnblocksParkedSender) {
    ParamGuard guard(true, 4);
    auto pipeline = ReplyCompressionPipeline::make();

    std::atomic<bool> returned{false};  // NOLINT
    std::atomic<bool> gotBatch{true};   // NOLINT
    stdx::thread sender([&] {
        Message out;
        // Nothing was submitted, so this only returns on shutdown.
        gotBatch.store(pipeline->takeNextReady(&out));
        returned.store(true);
    });

    sleepFor(Milliseconds(50));
    ASSERT_FALSE(returned.load());
    pipeline->shutdown();
    sender.join();
    ASSERT_TRUE(returned.load());
    ASSERT_FALSE(gotBatch.load());
}

// Tasks still queued at shutdown release their reservation when they run.
TEST_F(ReplCompressionPipelineTest, ShutdownWithQueuedWorkReleasesBudget) {
    ParamGuard guard(true, 64);
    auto pipeline = ReplyCompressionPipeline::make();

    for (int i = 0; i < 32; ++i) {
        Message in = makeMsg(i);
        ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
    }
    pipeline->shutdown();
    ASSERT_FALSE(pipeline->hasPending());
    ASSERT_TRUE(waitForBudgetToDrain());
    ASSERT_EQ(ReplyCompressionPipeline::globalPendingBytes(), 0);
}

// Destroying the pipeline (session gone) with queued work also releases the budget.
TEST_F(ReplCompressionPipelineTest, DestroyWithQueuedWorkReleasesBudget) {
    ParamGuard guard(true, 64);
    {
        auto pipeline = ReplyCompressionPipeline::make();
        for (int i = 0; i < 32; ++i) {
            Message in = makeMsg(i);
            ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
        }
    }
    ASSERT_TRUE(waitForBudgetToDrain());
}

TEST_F(ReplCompressionPipelineTest, BudgetReturnsToZeroAfterDrain) {
    ParamGuard guard(true, 16);
    auto pipeline = ReplyCompressionPipeline::make();
    for (int i = 0; i < 10; ++i) {
        Message in = makeMsg(i);
        ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
    }
    for (int i = 0; i < 10; ++i) {
        Message out;
        ASSERT_TRUE(pipeline->takeNextReady(&out));
        ASSERT_EQ(markerOf(out), i);
    }
    ASSERT_EQ(ReplyCompressionPipeline::globalPendingBytes(), 0);
}

TEST_F(ReplCompressionPipelineTest, WorkerExceptionPublishesFailureAndReleasesBudget) {
    ParamGuard guard(true, 4);
    auto pipeline = ReplyCompressionPipeline::make();
    const auto failures = pipelineMetric("compressFailures");
    const auto depth = pipelineMetric("rawQueueDepth");
    {
        FailPointEnableBlock failPoint("replPipelineCompressionThrows");
        Message in = makeMsg(0);
        ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
        Message out;
        ASSERT_THROWS_CODE(pipeline->takeNextReady(&out), DBException, ErrorCodes::InternalError);
    }
    ASSERT_FALSE(pipeline->hasPending());
    ASSERT_EQ(pipelineMetric("compressFailures"), failures + 1);
    ASSERT_EQ(pipelineMetric("rawQueueDepth"), depth);
    ASSERT_EQ(ReplyCompressionPipeline::globalPendingBytes(), 0);

    Message in = makeMsg(1);
    ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
    Message out;
    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_EQ(markerOf(out), 1);
}

TEST_F(ReplCompressionThreadPoolTest, ShutdownBeforeStartupNeverRestarts) {
    auto pool = makePool();
    pool->shutdown();
    pool->ensureStarted();
    int calls = 0;
    pool->schedule([&](Status status) {
        ASSERT_EQ(status.code(), ErrorCodes::ShutdownInProgress);
        ++calls;
        pool->schedule([&](Status nestedStatus) {
            ASSERT_EQ(nestedStatus.code(), ErrorCodes::ShutdownInProgress);
            ++calls;
        });
    });
    pool->shutdown();
    ASSERT_TRUE(pool->isShutdown());
    ASSERT_EQ(calls, 2);
}

TEST_F(ReplCompressionThreadPoolTest, ShutdownJoinsWorkersWithoutHoldingAdmissionMutex) {
    auto pool = makePool();
    pool->ensureStarted();
    std::atomic<bool> started{false};
    std::atomic<int> completed{0};
    std::atomic<int> unexpectedStatus{0};
    pool->schedule([&](Status status) {
        if (!status.isOK()) {
            unexpectedStatus.fetch_add(1);
        }
        started.store(true);
        while (!pool->isShutdown()) {
            std::this_thread::yield();
        }
        pool->schedule([&](Status nestedStatus) {
            if (nestedStatus.code() != ErrorCodes::ShutdownInProgress) {
                unexpectedStatus.fetch_add(1);
            }
            completed.fetch_add(1);
        });
        completed.fetch_add(1);
    });
    while (!started.load()) {
        std::this_thread::yield();
    }
    pool->shutdown();
    ASSERT_EQ(completed.load(), 2);
    ASSERT_EQ(unexpectedStatus.load(), 0);
}

TEST_F(ReplCompressionThreadPoolTest, ConcurrentScheduleAndShutdownCompleteEveryTaskOnce) {
    auto pool = makePool();
    pool->ensureStarted();
    constexpr int kSchedulers = 8;
    constexpr int kTasks = 100;
    std::atomic<bool> start{false};
    std::atomic<int> completed{0};
    std::atomic<int> unexpectedStatus{0};
    std::vector<stdx::thread> schedulers;
    for (int i = 0; i < kSchedulers; ++i) {
        schedulers.emplace_back([&] {
            while (!start.load()) {
                std::this_thread::yield();
            }
            for (int j = 0; j < kTasks; ++j) {
                pool->schedule([&](Status status) {
                    if (!status.isOK() && status.code() != ErrorCodes::ShutdownInProgress) {
                        unexpectedStatus.fetch_add(1);
                    }
                    completed.fetch_add(1);
                });
            }
        });
    }
    stdx::thread closer([&] {
        while (!start.load()) {
            std::this_thread::yield();
        }
        pool->shutdown();
    });
    start.store(true);
    pool->shutdown();
    for (auto& scheduler : schedulers) {
        scheduler.join();
    }
    closer.join();
    ASSERT_EQ(completed.load(), kSchedulers * kTasks);
    ASSERT_EQ(unexpectedStatus.load(), 0);
}

// --- awaitData wake hook ---------------------------------------------------------------------

// Installs a hook that records the last opCtx tag it was called with and how many times.
class WakeHookGuard {
public:
    WakeHookGuard() {
        _prevWake = gReplPipelineCompressionWakeAwaitData.load();
        gReplPipelineCompressionWakeAwaitData.store(true);
        sink().store(nullptr);
        count().store(0);
        setReplPipelineAwaitDataWakeFn(&WakeHookGuard::onWake);
    }
    ~WakeHookGuard() {
        setReplPipelineAwaitDataWakeFn(nullptr);
        gReplPipelineCompressionWakeAwaitData.store(_prevWake);
    }

    static void* lastTag() {
        return sink().load();
    }
    static int wakeCount() {
        return count().load();
    }

private:
    static void onWake(void* tag) {
        sink().store(tag);
        count().fetch_add(1);
    }
    static std::atomic<void*>& sink() {
        static std::atomic<void*> s{nullptr};
        return s;
    }
    static std::atomic<int>& count() {
        static std::atomic<int> c{0};
        return c;
    }
    bool _prevWake;
};

TEST_F(ReplCompressionPipelineTest, WakeFiresAtRegisteredOpCtx) {
    ParamGuard guard(true, 8);
    WakeHookGuard hook;
    auto pipeline = ReplyCompressionPipeline::make();

    int fakeOpCtx = 0;
    void* tag = &fakeOpCtx;
    pipeline->registerActiveOpCtx(tag);

    const long long wakes0 = pipelineMetric("awaitDataWakes");

    Message in = makeMsg(0);
    ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));

    for (int i = 0; i < 200000 && WakeHookGuard::lastTag() == nullptr; ++i) {
        std::this_thread::yield();
    }
    ASSERT_EQ(WakeHookGuard::lastTag(), tag);
    ASSERT_GT(pipelineMetric("awaitDataWakes"), wakes0);

    const int wakesAfterFirst = WakeHookGuard::wakeCount();
    Message out;
    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_GE(WakeHookGuard::wakeCount(), wakesAfterFirst);

    pipeline->clearActiveOpCtx();
}

TEST_F(ReplCompressionPipelineTest, WorkerExceptionWakesRegisteredOpCtx) {
    ParamGuard guard(true, 4);
    WakeHookGuard hook;
    auto pipeline = ReplyCompressionPipeline::make();
    int fakeOpCtx = 0;
    pipeline->registerActiveOpCtx(&fakeOpCtx);
    FailPointEnableBlock failPoint("replPipelineCompressionThrows");
    Message in = makeMsg(0);
    ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
    Message out;
    ASSERT_THROWS_CODE(pipeline->takeNextReady(&out), DBException, ErrorCodes::InternalError);
    pipeline->clearActiveOpCtx();
    ASSERT_EQ(WakeHookGuard::lastTag(), &fakeOpCtx);
    ASSERT_EQ(WakeHookGuard::wakeCount(), 1);
}

TEST_F(ReplCompressionPipelineTest, WorkerExceptionBeforeRegisterConvergesOnRegister) {
    ParamGuard guard(true, 4);
    WakeHookGuard hook;
    auto pipeline = ReplyCompressionPipeline::make();
    const auto missed = pipelineMetric("awaitDataWakeMissedNoTarget");
    FailPointEnableBlock failPoint("replPipelineCompressionThrows");
    Message in = makeMsg(0);
    ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));
    const auto deadline = Date_t::now() + Seconds(30);
    while (pipelineMetric("awaitDataWakeMissedNoTarget") == missed && Date_t::now() < deadline) {
        sleepFor(Milliseconds(1));
    }
    ASSERT_EQ(pipelineMetric("awaitDataWakeMissedNoTarget"), missed + 1);
    ASSERT_EQ(WakeHookGuard::wakeCount(), 0);
    int fakeOpCtx = 0;
    pipeline->registerActiveOpCtx(&fakeOpCtx);
    ASSERT_EQ(WakeHookGuard::lastTag(), &fakeOpCtx);
    ASSERT_EQ(WakeHookGuard::wakeCount(), 1);
    pipeline->clearActiveOpCtx();
    Message out;
    ASSERT_THROWS_CODE(pipeline->takeNextReady(&out), DBException, ErrorCodes::InternalError);
}

// A batch that finishes before any getMore registers is woken on the next registerActiveOpCtx().
TEST_F(ReplCompressionPipelineTest, LostWakeConvergesOnRegister) {
    ParamGuard guard(true, 8);
    WakeHookGuard hook;
    auto pipeline = ReplyCompressionPipeline::make();

    // No opCtx registered.
    const long long missed0 = pipelineMetric("awaitDataWakeMissedNoTarget");
    Message in = makeMsg(0);
    ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));

    for (int i = 0; i < 200000; ++i) {
        if (pipeline->hasPending() && pipelineMetric("awaitDataWakeMissedNoTarget") > missed0) {
            break;
        }
        std::this_thread::yield();
    }
    ASSERT_GT(pipelineMetric("awaitDataWakeMissedNoTarget"), missed0);
    ASSERT_EQ(WakeHookGuard::lastTag(), nullptr);  // no wake fired yet

    // Now register a getMore opCtx: convergence must re-fire the wake immediately at this opCtx.
    int fakeOpCtx = 0;
    void* tag = &fakeOpCtx;
    pipeline->registerActiveOpCtx(tag);
    ASSERT_EQ(WakeHookGuard::lastTag(), tag);

    Message out;
    ASSERT_TRUE(pipeline->takeNextReady(&out));
    ASSERT_EQ(markerOf(out), 0);
    pipeline->clearActiveOpCtx();
}

// The hook must never run on a tag after clearActiveOpCtx() returns. The tag is a heap object freed
// right after clear; a late hook call reads freed memory (caught by ASAN, or by the magic check).
namespace {
struct WakeProbe {
    static constexpr uint64_t kMagic = 0xC0FFEE1234567890ULL;
    uint64_t magic = kMagic;
    std::atomic<int> touches{0};
};

// A hook that treats the tag as a WakeProbe*, asserts it is still the live object, and mutates it.
std::atomic<bool> gProbeHookCorruption{false};
void probeWakeHook(void* tag) {
    auto* p = static_cast<WakeProbe*>(tag);
    if (p->magic != WakeProbe::kMagic) {
        gProbeHookCorruption.store(true);
    }
    p->touches.fetch_add(1);
}
}  // namespace

TEST_F(ReplCompressionPipelineTest, WakeNeverRunsAgainstClearedOpCtx) {
    ParamGuard guard(true, 8);
    const bool prevWake = gReplPipelineCompressionWakeAwaitData.load();
    gReplPipelineCompressionWakeAwaitData.store(true);
    setReplPipelineAwaitDataWakeFn(&probeWakeHook);
    gProbeHookCorruption.store(false);
    ON_BLOCK_EXIT([&] {
        setReplPipelineAwaitDataWakeFn(nullptr);
        gReplPipelineCompressionWakeAwaitData.store(prevWake);
    });

    constexpr int kIters = 500;
    for (int i = 0; i < kIters; ++i) {
        auto pipeline = ReplyCompressionPipeline::make();

        auto probe = std::make_unique<WakeProbe>();
        void* tag = probe.get();
        pipeline->registerActiveOpCtx(tag);

        Message in = makeMsg(i);
        ASSERT_TRUE(pipeline->trySubmit(in, snappyId()));

        // Races with the worker's wake.
        pipeline->clearActiveOpCtx();
        probe.reset();

        Message out;
        ASSERT_TRUE(pipeline->takeNextReady(&out));
        ASSERT_EQ(markerOf(out), i);
        pipeline->shutdown();
    }

    ASSERT_FALSE(gProbeHookCorruption.load())
        << "wake hook observed a destroyed/garbage opCtx target (use-after-free)";
}

}  // namespace
}  // namespace mongo::transport
