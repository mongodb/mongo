// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/receive_decode_pipeline.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_compressed.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/message_compressor_snappy.h"
#include "mongo/transport/message_compressor_zlib.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::repl {
namespace {

constexpr int kRequestId = 100;
constexpr int kFirstReplyId = 101;

long long pipelineMetric(std::string_view leaf) {
    BSONObjBuilder builder;
    globalMetricTreeSet()[ClusterRole::None].appendTo(builder);
    const auto obj = builder.obj();
    const auto element = obj["metrics"]["repl"]["network"]["receiveDecode"][leaf];
    return element.eoo() ? -1 : element.safeNumberLong();
}

bool waitForMetric(std::string_view leaf, long long expected) {
    const auto deadline = Date_t::now() + Seconds(30);
    while (pipelineMetric(leaf) != expected) {
        if (Date_t::now() >= deadline) {
            return false;
        }
        sleepFor(Milliseconds(1));
    }
    return true;
}

class ReceiveSession final : public transport::MockSession {
public:
    ReceiveSession() : MockSession(nullptr) {}

    void push(Message message) {
        std::lock_guard lk(_mutex);
        _messages.push_back(std::move(message));
        _cv.notify_all();
    }

    StatusWith<Message> sourceMessage() override {
        std::unique_lock lk(_mutex);
        ++_reads;
        _blocked = _messages.empty() && !_ended;
        _cv.notify_all();
        _cv.wait(lk, [&] { return _ended || !_messages.empty(); });
        _blocked = false;
        _cv.notify_all();
        if (_ended) {
            return Status(ErrorCodes::HostUnreachable, "Mock receive session closed");
        }
        auto message = std::move(_messages.front());
        _messages.pop_front();
        return message;
    }

    void end() override {
        std::lock_guard lk(_mutex);
        ++_ends;
        _ended = true;
        _cv.notify_all();
    }

    size_t ends() const {
        std::lock_guard lk(_mutex);
        return _ends;
    }

    bool isConnected() override {
        std::lock_guard lk(_mutex);
        return !_ended;
    }

    bool waitForBlockedRead(size_t reads) {
        std::unique_lock lk(_mutex);
        return _cv.wait_for(
            lk, std::chrono::seconds(30), [&] { return _reads >= reads && _blocked; });
    }

    bool waitForEnd() {
        std::unique_lock lk(_mutex);
        return _cv.wait_for(lk, std::chrono::seconds(30), [&] { return _ended; });
    }

    size_t reads() const {
        std::lock_guard lk(_mutex);
        return _reads;
    }

    size_t queued() const {
        std::lock_guard lk(_mutex);
        return _messages.size();
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    std::deque<Message> _messages;
    size_t _reads = 0;
    size_t _ends = 0;
    bool _blocked = false;
    bool _ended = false;
};

MessageCompressorId snappyId() {
    auto& registry = MessageCompressorRegistry::get();
    if (!registry.getCompressor("snappy")) {
        registry.setSupportedCompressors({"snappy", "zlib"});
        registry.registerImplementation(std::make_unique<SnappyMessageCompressor>());
        registry.registerImplementation(std::make_unique<ZlibMessageCompressor>());
        uassertStatusOK(registry.finalizeSupportedCompressors());
    }
    auto* compressor = registry.getCompressor("snappy");
    ASSERT(compressor);
    return compressor->getId();
}

Message compress(Message message) {
    MessageCompressorManager manager;
    const auto id = snappyId();
    auto compressed = uassertStatusOK(manager.compressMessage(message, &id));
    ASSERT_EQ(compressed.operation(), dbCompressed);
    return compressed;
}

Message makeReply(int marker, bool moreToCome, bool compressed = false) {
    OpMsgBuilder builder;
    builder.setBody(BSON("ok" << 1 << "marker" << marker << "pad" << std::string(4096, 'x')));
    auto message = builder.finish();
    message.header().setId(kFirstReplyId + marker);
    message.header().setResponseToMsgId(kRequestId + marker);
    if (moreToCome) {
        OpMsg::setFlag(&message, OpMsg::kMoreToCome);
    }
    return compressed ? compress(std::move(message)) : std::move(message);
}

class ReceiveDecodePipelineTest : public ServiceContextTest {
protected:
    void setUp() override {
        snappyId();
        _oldDepth = gReplReceiveDecodeQueueDepth.load();
        _oldBytes = gReplReceiveDecodeMaxPendingBytes.load();
        gReplReceiveDecodeQueueDepth.store(4);
        gReplReceiveDecodeMaxPendingBytes.store(256LL * 1024 * 1024);
    }

    void tearDown() override {
        for (auto& pipeline : _pipelines) {
            pipeline->stop(true);
        }
        _pipelines.clear();
        gReplReceiveDecodeQueueDepth.store(_oldDepth);
        gReplReceiveDecodeMaxPendingBytes.store(_oldBytes);
    }

    std::shared_ptr<ReceiveDecodePipeline> start(const std::shared_ptr<ReceiveSession>& session) {
        auto pipeline = ReceiveDecodePipeline::make(session, kRequestId);
        _pipelines.push_back(pipeline);
        return pipeline;
    }

    void assertReply(const ReceiveDecodePipeline::Reply& reply, int marker, bool moreToCome) {
        ASSERT_EQ(reply.message.operation(), dbMsg);
        ASSERT_EQ(reply.message.header().getId(), kFirstReplyId + marker);
        ASSERT_EQ(reply.message.header().getResponseToMsgId(), kRequestId + marker);
        ASSERT_EQ(OpMsg::isFlagSet(reply.message, OpMsg::kMoreToCome), moreToCome);
        ASSERT(reply.parsed);
        ASSERT(reply.reservation);
        ASSERT_EQ(reply.parsed->getCommandReply()["marker"].numberInt(), marker);
        ASSERT_EQ(reply.parsed->getCommandReply()["pad"].str(), std::string(4096, 'x'));
    }

private:
    int _oldDepth = 0;
    long long _oldBytes = 0;
    std::vector<std::shared_ptr<ReceiveDecodePipeline>> _pipelines;
};

TEST_F(ReceiveDecodePipelineTest, DeliversMixedSnappyAndUncompressedRepliesInWireOrder) {
    auto session = std::make_shared<ReceiveSession>();
    constexpr int kReplies = 12;
    for (int i = 0; i < kReplies; ++i) {
        session->push(makeReply(i, i != kReplies - 1, i % 2 == 0));
    }
    session->push(makeReply(kReplies, false));
    auto pipeline = start(session);

    for (int i = 0; i < kReplies; ++i) {
        auto reply = pipeline->take(kRequestId + i);
        assertReply(reply, i, i != kReplies - 1);
    }

    // Joining at the terminal reply makes the absence of another receive deterministic.
    pipeline->stop(false);
    ASSERT_EQ(session->reads(), static_cast<size_t>(kReplies));
    ASSERT_EQ(session->queued(), 1U);
    ASSERT_TRUE(session->isConnected());
}

TEST_F(ReceiveDecodePipelineTest, RetainedParsedReplyAndReservationSurviveStop) {
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, false, true));
    auto pipeline = start(session);
    auto reply = pipeline->take(kRequestId);
    pipeline->stop(false);
    assertReply(reply, 0, false);
    ASSERT_EQ(session->reads(), 1U);
}

TEST_F(ReceiveDecodePipelineTest, DeliveredReservationRetainsBudgetUntilReplyIsDestroyed) {
    const auto bytesBefore = pipelineMetric("pendingBytes");
    ASSERT_GTE(bytesBefore, 0);
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, false, true));
    auto pipeline = start(session);
    {
        auto reply = pipeline->take(kRequestId);
        pipeline->stop(false);
        assertReply(reply, 0, false);
        ASSERT_GT(pipelineMetric("pendingBytes"), bytesBefore);
        ASSERT_EQ(pipelineMetric("pendingBytes"),
                  bytesBefore + static_cast<long long>(reply.message.capacity()));
    }
    ASSERT_TRUE(waitForMetric("pendingBytes", bytesBefore));
}

TEST_F(ReceiveDecodePipelineTest, MinimumBudgetWaitResumesAfterRetainedReplyIsReleased) {
    const auto bytesBefore = pipelineMetric("pendingBytes");
    gReplReceiveDecodeMaxPendingBytes.store(2LL * MaxMessageSizeBytes);
    auto firstSession = std::make_shared<ReceiveSession>();
    firstSession->push(makeReply(0, false, true));
    auto first = start(firstSession);
    auto retained = first->take(kRequestId);
    ASSERT_FALSE(first->stop(false));

    auto secondSession = std::make_shared<ReceiveSession>();
    secondSession->push(makeReply(0, false, true));
    auto second = start(secondSession);
    int checks = 0;
    ASSERT_THROWS_CODE(second->take(kRequestId,
                                    [&] {
                                        return ++checks < 3
                                            ? Status::OK()
                                            : Status(ErrorCodes::ExceededTimeLimit,
                                                     "Interrupted while waiting for budget");
                                    }),
                       DBException,
                       ErrorCodes::ExceededTimeLimit);
    ASSERT_EQ(checks, 3);
    ASSERT_EQ(secondSession->reads(), 0U);
    assertReply(retained, 0, false);
    retained = ReceiveDecodePipeline::Reply{};
    {
        auto reply = second->take(kRequestId);
        assertReply(reply, 0, false);
        ASSERT_FALSE(second->stop(false));
    }
    ASSERT_EQ(pipelineMetric("pendingBytes"), bytesBefore);
}

TEST_F(ReceiveDecodePipelineTest, StopReleasesInternalRepliesBeforeWorkerCallbackReturns) {
    for (bool retainReply : {false, true}) {
        const auto bytesBefore = pipelineMetric("pendingBytes");
        const auto tasksBefore = pipelineMetric("activeDecodeTasks");
        const auto repliesBefore = pipelineMetric("pendingReplies");
        auto completed =
            std::make_unique<FailPointEnableBlock>("hangReplReceiveDecodeAfterTaskCompletion");
        auto session = std::make_shared<ReceiveSession>();
        std::weak_ptr<transport::Session> sessionRef = session;
        session->push(makeReply(0, false, true));
        auto pipeline = start(session);
        // Release the paused callback before cleanup if an assertion fails.
        ON_BLOCK_EXIT([&] {
            completed.reset();
            pipeline->stop(true);
        });
        auto opCtx = makeOperationContext();
        opCtx->runWithDeadline(Date_t::now() + Seconds(30), ErrorCodes::ExceededTimeLimit, [&] {
            (*completed)->waitForTimesEntered(opCtx.get(), completed->initialTimesEntered() + 1);
        });
        ASSERT_TRUE(waitForMetric("activeDecodeTasks", tasksBefore));
        ReceiveDecodePipeline::Reply reply;
        if (retainReply) {
            reply = pipeline->take(kRequestId);
        }
        session.reset();
        pipeline->stop(!retainReply);
        ASSERT_TRUE(sessionRef.expired());
        ASSERT_EQ(pipelineMetric("activeDecodeTasks"), tasksBefore);
        ASSERT_EQ(pipelineMetric("pendingReplies"), repliesBefore);
        ASSERT_EQ(pipelineMetric("pendingBytes"),
                  bytesBefore +
                      (retainReply ? static_cast<long long>(reply.message.capacity()) : 0));
        if (retainReply) {
            assertReply(reply, 0, false);
        }
        reply = ReceiveDecodePipeline::Reply{};
        ASSERT_EQ(pipelineMetric("pendingBytes"), bytesBefore);
        completed.reset();
    }
}

TEST_F(ReceiveDecodePipelineTest, DestroyStoppedPipelineLeavesHealthySessionConnected) {
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, false, true));
    auto pipeline = ReceiveDecodePipeline::make(session, kRequestId);
    ON_BLOCK_EXIT([&] {
        if (pipeline) {
            pipeline->stop(true);
        }
    });
    auto reply = pipeline->take(kRequestId);
    pipeline->stop(false);
    pipeline.reset();
    assertReply(reply, 0, false);
    ASSERT_TRUE(session->isConnected());
    ASSERT_EQ(session->reads(), 1U);
}

TEST_F(ReceiveDecodePipelineTest, CompletedSuccessorsWaitForSlowHeadAndAreDeliveredInOrder) {
    auto hang = std::make_unique<FailPointEnableBlock>("hangReplReceiveDecode", BSON("seq" << 0));
    const auto decodedBefore = pipelineMetric("repliesDecoded");
    const auto deliveredBefore = pipelineMetric("repliesDelivered");
    ASSERT_GTE(decodedBefore, 0);
    ASSERT_GTE(deliveredBefore, 0);
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, true, true));
    session->push(makeReply(1, true));
    session->push(makeReply(2, false, true));
    auto pipeline = start(session);
    std::mutex consumerMutex;
    std::condition_variable consumerCv;
    bool started = false;
    bool finished = false;
    Status consumerStatus = Status::OK();
    std::vector<ReceiveDecodePipeline::Reply> replies;
    stdx::thread consumer([&] {
        {
            std::lock_guard lk(consumerMutex);
            started = true;
            consumerCv.notify_all();
        }
        try {
            for (int i = 0; i < 3; ++i) {
                replies.push_back(pipeline->take(kRequestId + i));
            }
        } catch (const DBException& ex) {
            consumerStatus = ex.toStatus();
        }
        std::lock_guard lk(consumerMutex);
        finished = true;
        consumerCv.notify_all();
    });
    ON_BLOCK_EXIT([&] {
        if (consumer.joinable()) {
            pipeline->cancel();
            consumer.join();
        }
        pipeline->stop(true);
    });
    {
        std::unique_lock lk(consumerMutex);
        ASSERT_TRUE(consumerCv.wait_for(lk, std::chrono::seconds(30), [&] { return started; }));
    }
    auto opCtx = makeOperationContext();
    opCtx->runWithDeadline(Date_t::now() + Seconds(30), ErrorCodes::ExceededTimeLimit, [&] {
        (*hang)->waitForTimesEntered(opCtx.get(), hang->initialTimesEntered() + 1);
    });
    ASSERT_TRUE(waitForMetric("repliesDecoded", decodedBefore + 2));
    ASSERT_EQ(pipelineMetric("repliesDelivered"), deliveredBefore);
    {
        std::lock_guard lk(consumerMutex);
        ASSERT_FALSE(finished);
    }

    hang.reset();
    {
        std::unique_lock lk(consumerMutex);
        ASSERT_TRUE(consumerCv.wait_for(lk, std::chrono::seconds(30), [&] { return finished; }));
    }
    consumer.join();
    pipeline->stop(false);
    ASSERT_OK(consumerStatus);
    ASSERT_EQ(replies.size(), 3U);
    for (int i = 0; i < 3; ++i) {
        assertReply(replies[i], i, i != 2);
    }
    ASSERT_EQ(pipelineMetric("repliesDecoded"), decodedBefore + 3);
    ASSERT_EQ(pipelineMetric("repliesDelivered"), deliveredBefore + 3);
    ASSERT_EQ(session->reads(), 3U);
}

TEST_F(ReceiveDecodePipelineTest, WrongWireResponseToIsNotDelivered) {
    auto session = std::make_shared<ReceiveSession>();
    auto message = makeReply(0, false, true);
    message.header().setResponseToMsgId(kRequestId + 10);
    session->push(std::move(message));
    auto pipeline = start(session);
    ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, 40570);
    ASSERT_TRUE(session->waitForEnd());
    pipeline->stop(false);
    ASSERT_EQ(session->reads(), 1U);
    // The receiver's failed publish closes the session; the catch must not close it again.
    ASSERT_EQ(session->ends(), 1U);
}

TEST_F(ReceiveDecodePipelineTest, WrongCommittedResponseToIsNotDelivered) {
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, false, true));
    auto pipeline = start(session);
    ASSERT_THROWS_CODE(pipeline->take(kRequestId + 1), DBException, 40570);
    pipeline->stop(false);
    ASSERT_EQ(session->reads(), 1U);
}

TEST_F(ReceiveDecodePipelineTest, FailedHeadCannotBeSkippedForCompletedSuccessors) {
    auto hang = std::make_unique<FailPointEnableBlock>("hangReplReceiveDecode", BSON("seq" << 0));
    const auto decodedBefore = pipelineMetric("repliesDecoded");
    const auto deliveredBefore = pipelineMetric("repliesDelivered");
    auto session = std::make_shared<ReceiveSession>();
    auto badHead = makeReply(0, true);
    badHead.header().setLen(badHead.size() - 1);
    session->push(compress(std::move(badHead)));
    session->push(makeReply(1, true, true));
    session->push(makeReply(2, false, true));
    auto pipeline = start(session);
    ON_BLOCK_EXIT([&] { pipeline->stop(true); });
    auto opCtx = makeOperationContext();
    opCtx->runWithDeadline(Date_t::now() + Seconds(30), ErrorCodes::ExceededTimeLimit, [&] {
        (*hang)->waitForTimesEntered(opCtx.get(), hang->initialTimesEntered() + 1);
    });
    ASSERT_TRUE(waitForMetric("repliesDecoded", decodedBefore + 2));
    hang.reset();

    ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::InvalidBSON);
    ASSERT_THROWS_CODE(pipeline->take(kRequestId + 1), DBException, ErrorCodes::InvalidBSON);
    ASSERT_THROWS_CODE(pipeline->take(kRequestId + 2), DBException, ErrorCodes::InvalidBSON);
    ASSERT_EQ(pipelineMetric("repliesDelivered"), deliveredBefore);
    ASSERT_TRUE(pipeline->stop(false));
}

TEST_F(ReceiveDecodePipelineTest, SuccessBeforeFailedSuccessorIsStillDelivered) {
    auto hang = std::make_unique<FailPointEnableBlock>("hangReplReceiveDecode", BSON("seq" << 0));
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, true, true));
    auto badSuccessor = makeReply(1, false);
    badSuccessor.header().setLen(badSuccessor.size() - 1);
    session->push(compress(std::move(badSuccessor)));
    auto pipeline = start(session);
    ON_BLOCK_EXIT([&] { pipeline->stop(true); });
    auto opCtx = makeOperationContext();
    opCtx->runWithDeadline(Date_t::now() + Seconds(30), ErrorCodes::ExceededTimeLimit, [&] {
        (*hang)->waitForTimesEntered(opCtx.get(), hang->initialTimesEntered() + 1);
    });
    ASSERT_TRUE(session->waitForEnd());
    hang.reset();
    auto reply = pipeline->take(kRequestId);
    assertReply(reply, 0, true);
    ASSERT_THROWS_CODE(pipeline->take(kRequestId + 1), DBException, ErrorCodes::InvalidBSON);
    ASSERT_THROWS_CODE(pipeline->take(kRequestId + 2), DBException, ErrorCodes::InvalidBSON);
    ASSERT_TRUE(pipeline->stop(false));
}

TEST_F(ReceiveDecodePipelineTest, CommittedResponseMismatchPermanentlyStopsDelivery) {
    const auto decodedBefore = pipelineMetric("repliesDecoded");
    const auto deliveredBefore = pipelineMetric("repliesDelivered");
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, true, true));
    session->push(makeReply(1, false, true));
    auto pipeline = start(session);
    ASSERT_TRUE(waitForMetric("repliesDecoded", decodedBefore + 2));
    ASSERT_THROWS_CODE(pipeline->take(kRequestId + 10), DBException, 40570);
    ASSERT_FALSE(session->isConnected());
    ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, 40570);
    ASSERT_THROWS_CODE(pipeline->take(kRequestId + 1), DBException, 40570);
    ASSERT_EQ(pipelineMetric("repliesDelivered"), deliveredBefore);
    ASSERT_TRUE(pipeline->stop(false));
}

TEST_F(ReceiveDecodePipelineTest, InvalidBSONIsNotDelivered) {
    for (bool compressed : {false, true}) {
        auto session = std::make_shared<ReceiveSession>();
        auto message = makeReply(0, false);
        message.header().setLen(message.size() - 1);
        session->push(compressed ? compress(std::move(message)) : std::move(message));
        auto pipeline = start(session);
        ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::InvalidBSON);
        ASSERT_TRUE(session->waitForEnd());
        pipeline->stop(false);
        ASSERT_EQ(session->reads(), 1U);
    }
}

#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
TEST_F(ReceiveDecodePipelineTest, ValidChecksummedRepliesAreDeliveredInOrder) {
    for (bool compressed : {false, true}) {
        auto session = std::make_shared<ReceiveSession>();
        for (int i = 0; i < 2; ++i) {
            auto message = makeReply(i, i == 0);
            OpMsg::appendChecksum(&message);
            session->push(compressed ? compress(std::move(message)) : std::move(message));
        }
        auto pipeline = start(session);
        for (int i = 0; i < 2; ++i) {
            auto reply = pipeline->take(kRequestId + i);
            assertReply(reply, i, i == 0);
            ASSERT_TRUE(OpMsg::isFlagSet(reply.message, OpMsg::kChecksumPresent));
        }
        ASSERT_FALSE(pipeline->stop(false));
        ASSERT_TRUE(session->isConnected());
    }
}

TEST_F(ReceiveDecodePipelineTest, InvalidChecksumIsNotDelivered) {
    for (bool compressed : {false, true}) {
        auto session = std::make_shared<ReceiveSession>();
        auto message = makeReply(0, false);
        OpMsg::appendChecksum(&message);
        message.singleData().view2ptr()[message.size() - 1] ^= 1;
        session->push(compressed ? compress(std::move(message)) : std::move(message));
        auto pipeline = start(session);
        ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::ChecksumMismatch);
        ASSERT_TRUE(session->waitForEnd());
        pipeline->stop(false);
        ASSERT_EQ(session->reads(), 1U);
    }
}
#endif

TEST_F(ReceiveDecodePipelineTest, ValidSnappyFlagsPrefixDoesNotHideCorruptTail) {
    auto session = std::make_shared<ReceiveSession>();
    auto message = makeReply(0, false, true);
    message.header().setLen(message.size() - 1);
    const auto flags = readSnappyOpMsgFlags(message);
    ASSERT_OK(flags.getStatus());
    ASSERT_EQ(flags.getValue(), 0U);
    session->push(std::move(message));
    auto pipeline = start(session);
    ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::BadValue);
    ASSERT_TRUE(session->waitForEnd());
    pipeline->stop(false);
    ASSERT_EQ(session->reads(), 1U);
}

TEST_F(ReceiveDecodePipelineTest, IllegalFlagsAndCorruptTailKeepFullDecodeErrorPrecedence) {
    for (bool corruptTail : {false, true}) {
        const auto prefixFallbacksBefore = pipelineMetric("snappyPrefixFallbacks");
        const auto compressorFallbacksBefore = pipelineMetric("unsupportedCompressorFallbacks");
        auto original = makeReply(0, false);
        OpMsg::setFlag(&original, 1U << 2);
        auto message = compress(std::move(original));
        ASSERT_EQ(readSnappyOpMsgFlags(message).getStatus().code(), ErrorCodes::IllegalOpMsgFlag);
        if (corruptTail) {
            message.header().setLen(message.size() - 1);
        }
        auto session = std::make_shared<ReceiveSession>();
        session->push(std::move(message));
        auto pipeline = start(session);
        ASSERT_THROWS_CODE(pipeline->take(kRequestId),
                           DBException,
                           corruptTail ? ErrorCodes::BadValue : ErrorCodes::IllegalOpMsgFlag);
        ASSERT_TRUE(pipeline->stop(false));
        ASSERT_EQ(session->reads(), 1U);
        ASSERT_EQ(pipelineMetric("snappyPrefixFallbacks"), prefixFallbacksBefore);
        ASSERT_EQ(pipelineMetric("unsupportedCompressorFallbacks"), compressorFallbacksBefore);
    }
}

TEST_F(ReceiveDecodePipelineTest, NoncanonicalSnappyLiteralFallsBackToFullDecode) {
    for (bool moreToCome : {false, true}) {
        const auto prefixFallbacksBefore = pipelineMetric("snappyPrefixFallbacks");
        const auto compressorFallbacksBefore = pipelineMetric("unsupportedCompressorFallbacks");
        const auto failuresBefore = pipelineMetric("failures");
        const auto original = makeReply(0, moreToCome);
        const auto compressed = compress(original);
        const auto size = static_cast<size_t>(compressed.size());
        size_t prefixEnd = MsgData::MsgDataHeaderSize + CompressionHeader::size();
        const auto* data = reinterpret_cast<const uint8_t*>(compressed.sharedBuffer().get());
        while (true) {
            ASSERT_LT(prefixEnd, size);
            if (!(data[prefixEnd++] & 0x80)) {
                break;
            }
        }
        // libsnappy accepts a long-literal length that wraps to zero before the real payload.
        const std::array<uint8_t, 5> zeroLiteral{0xfc, 0xff, 0xff, 0xff, 0xff};
        auto buffer = SharedBuffer::allocate(size + zeroLiteral.size());
        std::memcpy(buffer.get(), data, prefixEnd);
        std::memcpy(buffer.get() + prefixEnd, zeroLiteral.data(), zeroLiteral.size());
        std::memcpy(
            buffer.get() + prefixEnd + zeroLiteral.size(), data + prefixEnd, size - prefixEnd);
        Message message(std::move(buffer));
        message.header().setLen(size + zeroLiteral.size());
        ASSERT_EQ(readSnappyOpMsgFlags(message).getStatus().code(), ErrorCodes::BadValue);
        MessageCompressorManager manager;
        auto fullDecode = uassertStatusOK(manager.decompressMessage(message));
        ASSERT_EQ(OpMsg::parse(fullDecode).body.woCompare(OpMsg::parse(original).body), 0);

        auto session = std::make_shared<ReceiveSession>();
        session->push(std::move(message));
        session->push(makeReply(1, false, true));
        auto pipeline = start(session);
        {
            auto reply = pipeline->take(kRequestId);
            assertReply(reply, 0, moreToCome);
            ASSERT_EQ(reply.endedReadAhead, moreToCome);
        }
        ASSERT_EQ(pipeline->stop(moreToCome), moreToCome);
        ASSERT_EQ(session->reads(), 1U);
        ASSERT_EQ(session->queued(), 1U);
        ASSERT_EQ(pipelineMetric("snappyPrefixFallbacks"), prefixFallbacksBefore + 1);
        ASSERT_EQ(pipelineMetric("unsupportedCompressorFallbacks"), compressorFallbacksBefore);
        ASSERT_EQ(pipelineMetric("failures"), failuresBefore);
    }
}

TEST_F(ReceiveDecodePipelineTest, WorkerDBExceptionIsPropagatedAndLaterPipelineStillWorks) {
    {
        FailPointEnableBlock failure("replReceiveDecodeThrows");
        auto session = std::make_shared<ReceiveSession>();
        session->push(makeReply(0, false, true));
        auto pipeline = start(session);
        ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::InternalError);
        // Failure-side close must finish before the error becomes consumable.
        ASSERT_FALSE(session->isConnected());
        pipeline->stop(false);
    }
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, false, true));
    auto pipeline = start(session);
    auto reply = pipeline->take(kRequestId);
    assertReply(reply, 0, false);
    pipeline->stop(false);
}

TEST_F(ReceiveDecodePipelineTest, DroppedTaskReleasesInputBudgetAndPublishesFailure) {
    const auto bytesBefore = pipelineMetric("pendingBytes");
    const auto tasksBefore = pipelineMetric("activeDecodeTasks");
    const auto failuresBefore = pipelineMetric("failures");
    FailPointEnableBlock failure("replReceiveDecodeScheduleThrows",
                                 BSON("errorCode" << int(ErrorCodes::ExceededTimeLimit)));
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, true, true));
    auto pipeline = start(session);
    try {
        pipeline->take(kRequestId);
        FAIL("Scheduling should have failed");
    } catch (const DBException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededTimeLimit);
        ASSERT_EQ(ex.toStatus().reason(), "Injected replication decode schedule failure");
    }
    ASSERT_FALSE(session->isConnected());
    pipeline->stop(false);
    ASSERT_EQ(pipelineMetric("pendingBytes"), bytesBefore);
    ASSERT_EQ(pipelineMetric("activeDecodeTasks"), tasksBefore);
    ASSERT_EQ(pipelineMetric("failures"), failuresBefore + 1);
    ASSERT_EQ(session->reads(), 1U);
}

TEST_F(ReceiveDecodePipelineTest, ScheduleThrowAfterQueuePublishesOnceAndJoinsTask) {
    const auto bytesBefore = pipelineMetric("pendingBytes");
    const auto tasksBefore = pipelineMetric("activeDecodeTasks");
    const auto failuresBefore = pipelineMetric("failures");
    FailPointEnableBlock hang("hangReplReceiveDecode");
    FailPointEnableBlock failure(
        "replReceiveDecodeScheduleThrows",
        BSON("afterSchedule" << true << "errorCode" << int(ErrorCodes::ExceededTimeLimit)));
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, true, true));
    auto pipeline = start(session);
    ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::ExceededTimeLimit);
    // The queued task is still owned and joins on cancellation even though its slot has an error.
    ASSERT_TRUE(pipeline->stop(true));
    ASSERT_EQ(pipelineMetric("pendingBytes"), bytesBefore);
    ASSERT_EQ(pipelineMetric("activeDecodeTasks"), tasksBefore);
    ASSERT_EQ(pipelineMetric("failures"), failuresBefore + 1);
}

TEST_F(ReceiveDecodePipelineTest, ScheduleThrowAfterSuccessfulDecodeStillClosesStream) {
    const auto bytesBefore = pipelineMetric("pendingBytes");
    const auto tasksBefore = pipelineMetric("activeDecodeTasks");
    const auto failuresBefore = pipelineMetric("failures");
    FailPointEnableBlock failure("replReceiveDecodeScheduleThrows",
                                 BSON("afterSchedule" << true << "waitForCompletion" << true
                                                      << "errorCode"
                                                      << int(ErrorCodes::ExceededTimeLimit)));
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, true, true));
    auto pipeline = start(session);
    {
        auto reply = pipeline->take(kRequestId);
        assertReply(reply, 0, true);
    }
    ASSERT_TRUE(session->waitForEnd());
    try {
        pipeline->take(kRequestId + 1);
        FAIL("The receiver's original error should follow the successful reply");
    } catch (const DBException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededTimeLimit);
        ASSERT_EQ(ex.toStatus().reason(), "Injected replication decode schedule failure");
    }
    ASSERT_TRUE(pipeline->stop(false));
    ASSERT_FALSE(session->isConnected());
    ASSERT_EQ(pipelineMetric("pendingBytes"), bytesBefore);
    ASSERT_EQ(pipelineMetric("activeDecodeTasks"), tasksBefore);
    ASSERT_EQ(pipelineMetric("failures"), failuresBefore);
}

TEST_F(ReceiveDecodePipelineTest, WorkerFailureDoesNotCountInducedReceiveError) {
    const auto failuresBefore = pipelineMetric("failures");
    auto hang = std::make_unique<FailPointEnableBlock>("hangReplReceiveDecode");
    FailPointEnableBlock failure("replReceiveDecodeThrows");
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, true, true));
    auto pipeline = start(session);
    ASSERT_TRUE(session->waitForBlockedRead(2));
    hang.reset();
    ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::InternalError);
    ASSERT_TRUE(pipeline->stop(false));
    ASSERT_EQ(pipelineMetric("failures"), failuresBefore + 1);
}

TEST_F(ReceiveDecodePipelineTest, HealthyMidStreamStopIsRejectedWithoutClosingSession) {
    auto session = std::make_shared<ReceiveSession>();
    auto pipeline = start(session);
    ASSERT_TRUE(session->waitForBlockedRead(1));
    ASSERT_THROWS_CODE(pipeline->stop(false), DBException, ErrorCodes::IllegalOperation);
    ASSERT_TRUE(session->isConnected());
    ASSERT_TRUE(pipeline->stop(true));
}

TEST_F(ReceiveDecodePipelineTest, CleanStopCannotDiscardUnconsumedTerminalReply) {
    const auto decodedBefore = pipelineMetric("repliesDecoded");
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, false, true));
    auto pipeline = start(session);
    ASSERT_TRUE(waitForMetric("repliesDecoded", decodedBefore + 1));
    ASSERT_THROWS_CODE(pipeline->stop(false), DBException, ErrorCodes::IllegalOperation);
    ASSERT_TRUE(session->isConnected());
    auto reply = pipeline->take(kRequestId);
    assertReply(reply, 0, false);
    ASSERT_FALSE(pipeline->stop(false));
}

TEST_F(ReceiveDecodePipelineTest, CancelInterruptsBlockedReceiveAndUnblocksTake) {
    const auto failuresBefore = pipelineMetric("failures");
    auto session = std::make_shared<ReceiveSession>();
    auto pipeline = start(session);
    Status consumerStatus = Status::OK();
    stdx::thread consumer([&] {
        try {
            pipeline->take(kRequestId);
            consumerStatus =
                Status(ErrorCodes::InternalError, "Unexpected reply after cancellation");
        } catch (const DBException& ex) {
            consumerStatus = ex.toStatus();
        }
    });
    ON_BLOCK_EXIT([&] {
        pipeline->cancel();
        if (consumer.joinable()) {
            consumer.join();
        }
        pipeline->stop(true);
    });

    ASSERT_TRUE(session->waitForBlockedRead(1));
    pipeline->cancel();
    consumer.join();
    pipeline->stop(true);
    ASSERT_EQ(consumerStatus.code(), ErrorCodes::CallbackCanceled);
    ASSERT_FALSE(session->isConnected());
    ASSERT_EQ(session->reads(), 1U);
    ASSERT_EQ(pipelineMetric("failures"), failuresBefore);
}

TEST_F(ReceiveDecodePipelineTest, CancelJoinsInFlightDecodeWithoutDisablingFailpoint) {
    const auto failuresBefore = pipelineMetric("failures");
    FailPointEnableBlock hang("hangReplReceiveDecode");
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, true, true));
    auto pipeline = start(session);
    ON_BLOCK_EXIT([&] { pipeline->stop(true); });
    auto opCtx = makeOperationContext();
    opCtx->runWithDeadline(Date_t::now() + Seconds(30), ErrorCodes::ExceededTimeLimit, [&] {
        hang->waitForTimesEntered(opCtx.get(), hang.initialTimesEntered() + 1);
    });
    ASSERT_TRUE(session->waitForBlockedRead(2));

    pipeline->cancel();
    pipeline->stop(true);
    ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::CallbackCanceled);
    ASSERT_FALSE(session->isConnected());
    ASSERT_EQ(session->reads(), 2U);
    ASSERT_EQ(pipelineMetric("failures"), failuresBefore);
}

TEST_F(ReceiveDecodePipelineTest, CloseExceptionPreservesDecodeErrorAndReturnsCloseRight) {
    class ThrowingEndSession final : public transport::MockSession {
    public:
        ThrowingEndSession() : MockSession(nullptr), _message(makeReply(0, false, true)) {}

        StatusWith<Message> sourceMessage() override {
            return std::move(_message);
        }

        void end() override {
            _ended.store(true);
            uasserted(ErrorCodes::HostUnreachable, "Injected session close failure");
        }

        bool isConnected() override {
            return !_ended.load();
        }

    private:
        Message _message;
        std::atomic<bool> _ended{false};
    };

    const auto failuresBefore = pipelineMetric("failures");
    FailPointEnableBlock failure("replReceiveDecodeThrows");
    auto session = std::make_shared<ThrowingEndSession>();
    auto pipeline = ReceiveDecodePipeline::make(session, kRequestId);
    ON_BLOCK_EXIT([&] { pipeline->stop(true); });
    try {
        pipeline->take(kRequestId);
        FAIL("Decoding should have failed");
    } catch (const DBException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::InternalError);
        ASSERT_EQ(ex.toStatus().reason(), "Injected replication decode failure");
    }
    ASSERT_TRUE(pipeline->stop(false));
    ASSERT_FALSE(session->isConnected());
    ASSERT_EQ(pipelineMetric("failures"), failuresBefore + 1);
}

TEST_F(ReceiveDecodePipelineTest, MoveAssignmentReleasesOldBuffersBeforeOldReservation) {
    // The reservation deleter observes whether the old message buffer is still referenced. With
    // member-wise assignment the reservation is replaced first, so the buffer would still be held.
    // The parsed view shares the same buffer, so it must be released first as well.
    auto oldMessage = makeReply(0, false);
    // The test keeps exactly one extra reference; the old message and parsed view hold the rest.
    const auto oldBuffer = oldMessage.sharedBuffer();
    bool bufferReleasedFirst = false;
    bool reservationReleased = false;
    ReceiveDecodePipeline::Reply reply;
    reply.message = std::move(oldMessage);
    reply.parsed = rpc::makeReply(&reply.message);
    ASSERT(reply.parsed);
    ASSERT_EQ(reply.parsed->getCommandReply()["marker"].numberInt(), 0);
    ASSERT_TRUE(oldBuffer.isShared());
    reply.reservation = std::shared_ptr<void>(new int(0), [&](int* p) {
        reservationReleased = true;
        // Only the test's reference remains if both the message and parsed view were destroyed.
        bufferReleasedFirst = !oldBuffer.isShared();
        delete p;
    });

    ReceiveDecodePipeline::Reply next;
    next.message = makeReply(1, false);
    next.parsed = rpc::makeReply(&next.message);
    next.reservation = std::make_shared<int>(1);

    reply = std::move(next);
    ASSERT_TRUE(reservationReleased);
    ASSERT_TRUE(bufferReleasedFirst);
    ASSERT_EQ(reply.message.header().getId(), kFirstReplyId + 1);
    ASSERT(reply.parsed);
    ASSERT_EQ(reply.parsed->getCommandReply()["marker"].numberInt(), 1);
    ASSERT(reply.reservation);
}

TEST_F(ReceiveDecodePipelineTest, LateCancelAfterStopDoesNotCloseHandedBackSession) {
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, false, true));
    auto pipeline = start(session);
    auto reply = pipeline->take(kRequestId);
    assertReply(reply, 0, false);
    ASSERT_FALSE(pipeline->stop(false));
    ASSERT_TRUE(session->isConnected());

    // Another owner still holds the old pipeline and cancels it after the session was returned.
    pipeline->cancel();
    ASSERT_TRUE(session->isConnected());
    ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::CallbackCanceled);
    ASSERT_TRUE(session->isConnected());
    // The handoff was healthy; a later cancel() must not rewrite that result.
    ASSERT_FALSE(pipeline->stop(false));
}

TEST_F(ReceiveDecodePipelineTest, StopWaitsForConcurrentCancelToFinishClosing) {
    class SlowEndSession final : public transport::MockSession {
    public:
        SlowEndSession() : MockSession(nullptr) {}

        void push(Message message) {
            std::lock_guard lk(_mutex);
            _messages.push_back(std::move(message));
            _cv.notify_all();
        }

        StatusWith<Message> sourceMessage() override {
            std::unique_lock lk(_mutex);
            _cv.wait(lk, [&] { return _ended || !_messages.empty(); });
            if (_ended) {
                return Status(ErrorCodes::HostUnreachable, "Mock receive session closed");
            }
            auto message = std::move(_messages.front());
            _messages.pop_front();
            return message;
        }

        void end() override {
            std::unique_lock lk(_mutex);
            _inEnd = true;
            _cv.notify_all();
            _cv.wait(lk, [&] { return _releaseEnd; });
            _ended = true;
            _inEnd = false;
            _cv.notify_all();
        }

        bool isConnected() override {
            std::lock_guard lk(_mutex);
            return !_ended;
        }

        bool waitForEndStarted() {
            std::unique_lock lk(_mutex);
            return _cv.wait_for(lk, std::chrono::seconds(30), [&] { return _inEnd; });
        }

        void releaseEnd() {
            std::lock_guard lk(_mutex);
            _releaseEnd = true;
            _cv.notify_all();
        }

        bool endFinished() const {
            std::lock_guard lk(_mutex);
            return _ended && !_inEnd;
        }

    private:
        mutable std::mutex _mutex;
        std::condition_variable _cv;
        std::deque<Message> _messages;
        bool _inEnd = false;
        bool _releaseEnd = false;
        bool _ended = false;
    };

    auto session = std::make_shared<SlowEndSession>();
    session->push(makeReply(0, false, true));
    auto pipeline = ReceiveDecodePipeline::make(session, kRequestId);
    // Declared before the guard so they outlive the threads joined by it on any failure path.
    auto revoked =
        std::make_unique<FailPointEnableBlock>("hangReplReceiveDecodeAfterRevokingClose");
    bool stopped = false;
    bool stopReportedInterrupted = false;
    std::mutex stoppedMutex;
    std::condition_variable stoppedCv;
    stdx::thread canceler;
    stdx::thread stopper;
    ON_BLOCK_EXIT([&] {
        revoked.reset();
        session->releaseEnd();
        if (canceler.joinable()) {
            canceler.join();
        }
        if (stopper.joinable()) {
            stopper.join();
        }
        pipeline->stop(true);
    });
    auto reply = pipeline->take(kRequestId);
    assertReply(reply, 0, false);

    // The canceler acquires the close right and blocks inside end().
    canceler = stdx::thread([&] { pipeline->cancel(); });
    ASSERT_TRUE(session->waitForEndStarted());

    stopper = stdx::thread([&] {
        const bool interrupted = pipeline->stop(false);
        std::lock_guard lk(stoppedMutex);
        stopReportedInterrupted = interrupted;
        stopped = true;
        stoppedCv.notify_all();
    });

    // Deterministic handshake: stop() has revoked the close right and is about to wait for the
    // in-flight close. Only then release the failpoint and prove that stop() keeps waiting.
    auto opCtx = makeOperationContext();
    opCtx->runWithDeadline(Date_t::now() + Seconds(30), ErrorCodes::ExceededTimeLimit, [&] {
        (*revoked)->waitForTimesEntered(opCtx.get(), revoked->initialTimesEntered() + 1);
    });
    revoked.reset();
    {
        std::unique_lock lk(stoppedMutex);
        ASSERT_FALSE(
            stoppedCv.wait_for(lk, std::chrono::milliseconds(200), [&] { return stopped; }));
    }
    ASSERT_FALSE(session->endFinished());

    session->releaseEnd();
    stopper.join();
    canceler.join();
    ASSERT_TRUE(stopped);
    // The concurrent cancel won the close right, so the handed-back session is closed and stop()
    // must say so instead of reporting a healthy handoff.
    ASSERT_TRUE(stopReportedInterrupted);
    ASSERT_TRUE(session->endFinished());
}

TEST_F(ReceiveDecodePipelineTest, InterruptCheckEndsWaitForStuckHead) {
    FailPointEnableBlock hang("hangReplReceiveDecode");
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, true, true));
    auto pipeline = start(session);
    auto opCtx = makeOperationContext();
    opCtx->runWithDeadline(Date_t::now() + Seconds(30), ErrorCodes::ExceededTimeLimit, [&] {
        hang->waitForTimesEntered(opCtx.get(), hang.initialTimesEntered() + 1);
    });

    // The head reply stays stuck in decode; only the owner's check can end the wait.
    std::atomic<int> checks{0};  // NOLINT
    ASSERT_THROWS_CODE(pipeline->take(kRequestId,
                                      [&] {
                                          return ++checks < 3
                                              ? Status::OK()
                                              : Status(ErrorCodes::CallbackCanceled, "disabled");
                                      }),
                       DBException,
                       ErrorCodes::CallbackCanceled);
    ASSERT_EQ(checks.load(), 3);
    // The check only ends the wait; the owner still stops the pipeline.
    ASSERT_TRUE(session->isConnected());
    ASSERT_TRUE(pipeline->stop(true));
    ASSERT_FALSE(session->isConnected());
}

TEST_F(ReceiveDecodePipelineTest, PassingInterruptCheckDoesNotAffectDelivery) {
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, true, true));
    session->push(makeReply(1, false, true));
    auto pipeline = start(session);
    for (int i = 0; i < 2; ++i) {
        auto reply = pipeline->take(kRequestId + i, [] { return Status::OK(); });
        assertReply(reply, i, i == 0);
    }
    ASSERT_FALSE(pipeline->stop(false));
    ASSERT_TRUE(session->isConnected());
}

// A frame compressed with another negotiated compressor is decoded and validated in full. Its
// flags can't be read ahead, so the receiver stops after it and the reply says so.
TEST_F(ReceiveDecodePipelineTest, NonSnappyFrameIsDecodedAndEndsReadAhead) {
    const auto prefixFallbacksBefore = pipelineMetric("snappyPrefixFallbacks");
    const auto compressorFallbacksBefore = pipelineMetric("unsupportedCompressorFallbacks");
    const auto failuresBefore = pipelineMetric("failures");
    auto* zlib = MessageCompressorRegistry::get().getCompressor("zlib");
    ASSERT(zlib);
    MessageCompressorManager manager;
    const auto id = zlib->getId();
    auto compressed = uassertStatusOK(manager.compressMessage(makeReply(0, true), &id));
    ASSERT_EQ(compressed.operation(), dbCompressed);

    auto session = std::make_shared<ReceiveSession>();
    session->push(std::move(compressed));
    session->push(makeReply(1, false, true));
    auto pipeline = start(session);
    auto reply = pipeline->take(kRequestId);
    assertReply(reply, 0, true);
    ASSERT_TRUE(reply.endedReadAhead);
    // The next frame was left unread for the synchronous path.
    ASSERT_TRUE(pipeline->stop(true));
    ASSERT_EQ(session->reads(), 1U);
    ASSERT_EQ(session->queued(), 1U);
    ASSERT_EQ(pipelineMetric("snappyPrefixFallbacks"), prefixFallbacksBefore);
    ASSERT_EQ(pipelineMetric("unsupportedCompressorFallbacks"), compressorFallbacksBefore + 1);
    ASSERT_EQ(pipelineMetric("failures"), failuresBefore);
}

// Oplog getMore replies are a single body section. A document sequence is rejected before the
// parser materializes one BSONObj per document outside the receive budget.
TEST_F(ReceiveDecodePipelineTest, DocumentSequenceIsRejectedBeforeParsing) {
    for (bool compressed : {false, true}) {
        OpMsgBuilder builder;
        {
            auto seq = builder.beginDocSequence("docs");
            for (int i = 0; i < 1000; ++i) {
                seq.append(BSONObj());
            }
        }
        builder.setBody(BSON("ok" << 1 << "marker" << 0));
        auto message = builder.finish();
        message.header().setId(kFirstReplyId);
        message.header().setResponseToMsgId(kRequestId);

        auto session = std::make_shared<ReceiveSession>();
        session->push(compressed ? compress(std::move(message)) : std::move(message));
        auto pipeline = start(session);
        ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::UnsupportedFormat);
        ASSERT_TRUE(session->waitForEnd());
        pipeline->stop(false);
    }
}

// Errors before the first document sequence keep the same precedence as OpMsg::parse.
TEST_F(ReceiveDecodePipelineTest, DocumentSequenceRejectionKeepsParserErrorPrecedence) {
    // Unknown required flags are reported before any section, as by OpMsg::parse.
    for (bool compressed : {false, true}) {
        OpMsgBuilder builder;
        {
            auto seq = builder.beginDocSequence("docs");
            seq.append(BSONObj());
        }
        builder.setBody(BSON("ok" << 1 << "marker" << 0));
        auto message = builder.finish();
        message.header().setId(kFirstReplyId);
        message.header().setResponseToMsgId(kRequestId);
        OpMsg::setFlag(&message, 1U << 2);
        ASSERT_THROWS_CODE(OpMsg::parse(message), DBException, ErrorCodes::IllegalOpMsgFlag);

        auto session = std::make_shared<ReceiveSession>();
        session->push(compressed ? compress(std::move(message)) : std::move(message));
        auto pipeline = start(session);
        ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::IllegalOpMsgFlag);
        ASSERT_TRUE(session->waitForEnd());
        pipeline->stop(false);
    }

    // An invalid body before the sequence is reported first, as by OpMsg::parse.
    for (bool compressed : {false, true}) {
        BufBuilder buf;
        buf.skip(MsgData::MsgDataHeaderSize);
        buf.appendNum(static_cast<uint32_t>(0));  // flags
        buf.appendChar(0);                        // body section
        const char badBody[] = {5, 0, 0, 0, 1};   // not terminated with EOO
        buf.appendBuf(badBody, sizeof(badBody));
        buf.appendChar(1);  // document sequence section
        buf.appendNum(static_cast<int32_t>(sizeof(int32_t) + 5));
        buf.appendCStr("docs");
        const auto size = buf.len();
        Message message(buf.release());
        message.header().setLen(size);
        message.header().setOperation(dbMsg);
        message.header().setId(kFirstReplyId);
        message.header().setResponseToMsgId(kRequestId);
        ASSERT_THROWS_CODE(OpMsg::parse(message), DBException, ErrorCodes::InvalidBSON);

        auto session = std::make_shared<ReceiveSession>();
        session->push(compressed ? compress(std::move(message)) : std::move(message));
        auto pipeline = start(session);
        ASSERT_THROWS_CODE(pipeline->take(kRequestId), DBException, ErrorCodes::InvalidBSON);
        ASSERT_TRUE(session->waitForEnd());
        pipeline->stop(false);
    }
}

TEST_F(ReceiveDecodePipelineTest, CleanStopReportsHealthyHandoff) {
    auto session = std::make_shared<ReceiveSession>();
    session->push(makeReply(0, false, true));
    auto pipeline = start(session);
    auto reply = pipeline->take(kRequestId);
    assertReply(reply, 0, false);
    ASSERT_FALSE(pipeline->stop(false));
    ASSERT_TRUE(session->isConnected());
    // A repeated stop must keep reporting the same healthy result.
    ASSERT_FALSE(pipeline->stop(false));
}

}  // namespace
}  // namespace mongo::repl
