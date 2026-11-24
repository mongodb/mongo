// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/receive_decode_pipeline.h"

#include "mongo/base/counter.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/op_compressed.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/message_compressor_snappy.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/timer.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo::repl {
namespace {

MONGO_FAIL_POINT_DEFINE(replReceiveDecodeThrows);
MONGO_FAIL_POINT_DEFINE(replReceiveDecodeScheduleThrows);
MONGO_FAIL_POINT_DEFINE(hangReplReceiveDecode);
// Test-only: pauses stop() after it revoked the close right and before it waits for closes that
// already acquired it. Lets tests prove that stop() reached the wait rather than being unscheduled.
MONGO_FAIL_POINT_DEFINE(hangReplReceiveDecodeAfterRevokingClose);
MONGO_FAIL_POINT_DEFINE(hangReplReceiveDecodeAfterTaskCompletion);

// Reserve both maximum input and output before sourceMessage can allocate either one. The
// reservation is subsequently shrunk, never increased, so inputs cannot starve the oldest decoder.
constexpr size_t kReceiveReservation = 2 * MaxMessageSizeBytes;

auto& received = *MetricBuilder<Counter64>{"repl.network.receiveDecode.repliesReceived"};
auto& decoded = *MetricBuilder<Counter64>{"repl.network.receiveDecode.repliesDecoded"};
auto& delivered = *MetricBuilder<Counter64>{"repl.network.receiveDecode.repliesDelivered"};
auto& failures = *MetricBuilder<Counter64>{"repl.network.receiveDecode.failures"};
// Successfully validated replies whose flags required full decoding instead of prefix read-ahead.
// These are not failures; a continuing reply makes the owner resume without read-ahead.
auto& snappyPrefixFallbacks =
    *MetricBuilder<Counter64>{"repl.network.receiveDecode.snappyPrefixFallbacks"};
auto& unsupportedCompressorFallbacks =
    *MetricBuilder<Counter64>{"repl.network.receiveDecode.unsupportedCompressorFallbacks"};
// Signed atomic counters used as gauges; every admission has a matching release.
auto& pendingBytes = *MetricBuilder<Counter64>{"repl.network.receiveDecode.pendingBytes"};
auto& pendingReplies = *MetricBuilder<Counter64>{"repl.network.receiveDecode.pendingReplies"};
auto& activeTasks = *MetricBuilder<Counter64>{"repl.network.receiveDecode.activeDecodeTasks"};
auto& recvMicros = *MetricBuilder<Counter64>{"repl.network.receiveDecode.recvMicros"};
auto& decodeMicros = *MetricBuilder<Counter64>{"repl.network.receiveDecode.decompressMicros"};
auto& validateMicros = *MetricBuilder<Counter64>{"repl.network.receiveDecode.validateMicros"};
auto& orderedWaitMicros = *MetricBuilder<Counter64>{"repl.network.receiveDecode.orderedWaitMicros"};

struct Budget {
    std::mutex mutex;
    stdx::condition_variable cv;
    size_t used = 0;

    static Budget& get() {
        static StaticImmortal<Budget> budget;
        return *budget;
    }
};

class Reservation {
public:
    explicit Reservation(size_t bytes) : _bytes(bytes) {}
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation() {
        shrink(0);
    }

    static std::shared_ptr<Reservation> acquire(const std::atomic<bool>& canceled,
                                                const std::atomic<bool>& failed) {
        auto result = std::make_shared<Reservation>(0);
        auto& budget = Budget::get();
        std::unique_lock lk(budget.mutex);
        const auto limit = static_cast<size_t>(gReplReceiveDecodeMaxPendingBytes.load());
        budget.cv.wait(lk, [&] {
            return canceled.load() || failed.load() ||
                (budget.used <= limit && kReceiveReservation <= limit - budget.used);
        });
        if (canceled.load() || failed.load()) {
            return nullptr;
        }
        budget.used += kReceiveReservation;
        result->_bytes = kReceiveReservation;
        pendingBytes.increment(kReceiveReservation);
        return result;
    }

    void shrink(size_t bytes) {
        invariant(bytes <= _bytes);
        auto& budget = Budget::get();
        std::lock_guard lk(budget.mutex);
        const auto released = _bytes - bytes;
        budget.used -= released;
        pendingBytes.decrement(released);
        _bytes = bytes;
        budget.cv.notify_all();
    }

private:
    size_t _bytes;
};

class DecodePool {
public:
    static DecodePool& get() {
        static StaticImmortal<DecodePool> pool;
        return *pool;
    }

    void start() {
        std::lock_guard lk(_mutex);
        uassert(
            ErrorCodes::ShutdownInProgress, "Replication decode pool is shutting down", !_stopped);
        if (!_pool) {
            ThreadPool::Options options;
            options.poolName = "ReplDecodePool";
            options.threadNamePrefix = "ReplDecode-";
            options.minThreads = 1;
            options.maxThreads = gReplReceiveDecodeThreadPoolSize.load();
            auto pool = std::make_shared<ThreadPool>(options);
            pool->startup();
            // ThreadPool logs and swallows a failure to allocate a worker. Publishing a pool with
            // no worker would accept tasks that never run, so stop() could wait forever.
            if (pool->getStats().numThreads == 0) {
                pool->shutdown();
                pool->join();
                uasserted(ErrorCodes::InternalError,
                          "Replication decode pool could not start a worker thread");
            }
            _pool = std::move(pool);
        }
    }

    void schedule(unique_function<void(Status)> task) {
        std::shared_ptr<ThreadPool> pool;
        {
            std::lock_guard lk(_mutex);
            if (!_stopped && _pool) {
                pool = _pool;
                ++_schedulers;
            }
        }
        if (!pool) {
            task(Status(ErrorCodes::ShutdownInProgress, "Replication decode pool is shut down"));
            return;
        }
        ON_BLOCK_EXIT([&] {
            std::lock_guard lk(_mutex);
            --_schedulers;
            _cv.notify_all();
        });
        pool->schedule(std::move(task));
    }

    void shutdown() {
        std::unique_lock lk(_mutex);
        if (_stopped) {
            _cv.wait(lk, [&] { return _joined; });
            return;
        }
        _stopped = true;
        auto pool = _pool;
        lk.unlock();
        if (pool) {
            pool->shutdown();
        }
        lk.lock();
        _cv.wait(lk, [&] { return _schedulers == 0; });
        lk.unlock();
        if (pool) {
            pool->join();
        }
        lk.lock();
        _pool.reset();
        _joined = true;
        _cv.notify_all();
    }

private:
    std::mutex _mutex;
    stdx::condition_variable _cv;
    std::shared_ptr<ThreadPool> _pool;
    bool _stopped = false;
    bool _joined = false;
    size_t _schedulers = 0;
};

struct PrefixFlags {
    uint32_t flags = 0;
    // False for compressors whose flags cannot be read without decoding the whole frame.
    bool known = true;
};

PrefixFlags prefixFlags(const Message& message) {
    if (message.operation() == dbCompressed) {
        ConstDataRangeCursor input(message.header().data(),
                                   message.header().data() + message.header().dataLen());
        uassert(ErrorCodes::BadValue,
                "Invalid compressed message header",
                input.length() >= CompressionHeader::size());
        const CompressionHeader header(&input);
        if (header.compressorId == static_cast<MessageCompressorId>(MessageCompressor::kSnappy)) {
            const auto flags = readSnappyOpMsgFlags(message);
            if (flags.isOK()) {
                return {flags.getValue(), true};
            }
            // Full decoding decides validity and error precedence, including noncanonical
            // encodings accepted by libsnappy and illegal flags followed by a corrupt tail.
        }
        // Unknown flags stop read-ahead at this frame. The worker decodes and validates it in
        // full before the owner decides whether to hand back or abandon the stream.
        uassert(ErrorCodes::UnsupportedFormat,
                "Replication decode pipeline requires a compressed OP_MSG",
                header.originalOpCode == dbMsg);
        return {0, false};
    }
    uassert(ErrorCodes::UnsupportedFormat,
            "Replication decode pipeline requires OP_MSG or OP_COMPRESSED",
            message.operation() == dbMsg);
    const auto flags = OpMsg::flags(message);
    uassert(ErrorCodes::IllegalOpMsgFlag,
            "Unknown required OP_MSG flags",
            !OpMsg::hasUnknownRequiredFlags(flags));
    return {flags, true};
}

size_t outputSize(const Message& message) {
    if (message.operation() != dbCompressed) {
        return 0;
    }
    ConstDataRangeCursor input(message.header().data(),
                               message.header().data() + message.header().dataLen());
    CompressionHeader header(&input);
    return static_cast<size_t>(header.uncompressedSize) + MsgData::MsgDataHeaderSize;
}

}  // namespace

struct ReceiveDecodePipeline::State : std::enable_shared_from_this<State> {
    struct Slot {
        uint64_t seq;
        bool publishing = false;
        bool done = false;
        Status status = Status::OK();
        Reply reply;
    };

    explicit State(std::shared_ptr<transport::Session> session, int expectedResponseTo)
        : session(std::move(session)), expectedResponseTo(expectedResponseTo) {}

    void cancel() noexcept {
        {
            std::lock_guard lk(mutex);
            canceled.store(true);
            cv.notify_all();
        }
        endSessionIfOwned();
        notifyBudgetWaiters();
    }

    void stopReading() noexcept {
        {
            std::lock_guard lk(mutex);
            failed.store(true);
            cv.notify_all();
        }
        endSessionIfOwned();
        notifyBudgetWaiters();
    }

    /**
     * Closes the session only while this pipeline still owns it. The ownership check and the
     * in-flight count are updated under the same mutex, so detachSession() cannot return between
     * a caller's check and its end() call.
     */
    void endSessionIfOwned() noexcept {
        {
            std::lock_guard lk(mutex);
            if (!ownsSession) {
                return;
            }
            ++sessionClosers;
        }
        ON_BLOCK_EXIT([&] {
            std::lock_guard lk(mutex);
            if (--sessionClosers == 0) {
                cv.notify_all();
            }
        });
        try {
            session->end();
        } catch (...) {
            // Preserve the original receive/decode status and always return the close right.
            // Session implementations must still interrupt outstanding I/O when end() fails.
            LOGV2_ERROR(11369901,
                        "Error closing the replication receive decode session",
                        "error"_attr = redact(exceptionToStatus()));
        }
    }

    /**
     * Revokes this pipeline's right to close the session and waits for closes already started.
     * After this returns, a late cancel() or failure cannot end the session handed back to the
     * client. Returns whether the pipeline was canceled or failed by the time the right was
     * revoked; the session may then already be closed.
     */
    bool detachSession() {
        bool interrupted;
        {
            std::lock_guard lk(mutex);
            ownsSession = false;
            interrupted = canceled.load() || failed.load();
        }
        // The close right is revoked; only closes that already acquired it remain. Never pause
        // while holding the mutex, which those closers need in order to finish.
        hangReplReceiveDecodeAfterRevokingClose.pauseWhileSet();
        std::unique_lock lk(mutex);
        cv.wait(lk, [&] { return sessionClosers == 0; });
        auto detachedSession = std::move(session);
        lk.unlock();
        detachedSession.reset();
        return interrupted;
    }

    static void notifyBudgetWaiters() noexcept {
        auto& budget = Budget::get();
        std::lock_guard lk(budget.mutex);
        budget.cv.notify_all();
    }

    /**
     * Publishes the slot's result once. Returns false if another publisher already won, in which
     * case this call had no side effects. A failed status that wins has already stopped reading.
     */
    bool publish(const std::shared_ptr<Slot>& slot,
                 Status status,
                 Reply reply,
                 bool receiveNetworkError = false) {
        bool countFailure = false;
        {
            std::lock_guard lk(mutex);
            // schedule() may throw after queueing a task. Only one of its catch or the worker can
            // publish the result; a losing publisher must not repeat failure-side effects.
            if (slot->publishing || slot->done) {
                return false;
            }
            slot->publishing = true;
            if (!status.isOK()) {
                // Not decode faults: cancellation, a decode pool already shut down for process
                // exit, and network errors caused by our own cancel/failure closing the session.
                countFailure = status.code() != ErrorCodes::CallbackCanceled &&
                    status.code() != ErrorCodes::ShutdownInProgress &&
                    !(receiveNetworkError && (canceled.load() || failed.load()));
                failed.store(true);
            }
        }
        if (!status.isOK()) {
            if (countFailure) {
                failures.increment();
            }
            // Complete failure-side I/O and notifications before exposing task completion.
            stopReading();
        }
        {
            std::lock_guard lk(mutex);
            slot->status = std::move(status);
            slot->reply = std::move(reply);
            slot->done = true;
            cv.notify_all();
        }
        return true;
    }

    struct Completion {
        std::shared_ptr<State> state;
        std::shared_ptr<Slot> slot;
        bool registered = false;
        ~Completion() {
            auto keepAlive = std::move(state);
            // Release the task's reply reference before exposing completion to stop(). The last
            // slot reference may release a reservation and acquire the budget mutex.
            slot.reset();
            if (registered) {
                std::lock_guard lk(keepAlive->mutex);
                invariant(keepAlive->tasks > 0);
                --keepAlive->tasks;
                activeTasks.decrement(1);
                keepAlive->cv.notify_all();
            }
        }
    };

    struct DecodeTask {
        // Destruction order is explicit: input, budget, then task-lifetime completion.
        // Lambda capture-member ordering must not determine when the budget becomes reusable.
        std::shared_ptr<Completion> completion;
        std::shared_ptr<Reservation> reservation;
        Message raw;
        uint32_t flags = 0;
        bool prefixKnown = true;
    };

    void decode(std::shared_ptr<Slot> slot,
                Message raw,
                uint32_t flags,
                bool prefixKnown,
                std::shared_ptr<Reservation> reservation,
                std::shared_ptr<Completion> completion,
                Status dispatchStatus) {
        Reply reply;
        Status status = std::move(dispatchStatus);
        try {
            if (status.isOK()) {
                uassert(
                    ErrorCodes::CallbackCanceled, "Replication decode canceled", !canceled.load());
                // The failpoint must not prevent shutdown from completing.
                while (hangReplReceiveDecode.shouldFail([&](const BSONObj& data) {
                    return !data.hasField("seq") ||
                        data["seq"].safeNumberLong() == static_cast<long long>(slot->seq);
                }) && !canceled.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                uassert(
                    ErrorCodes::CallbackCanceled, "Replication decode canceled", !canceled.load());
                replReceiveDecodeThrows.execute([](const BSONObj&) {
                    uasserted(ErrorCodes::InternalError, "Injected replication decode failure");
                });
                Timer decompressTimer;
                bool snappyPrefixFallback = false;
                if (raw.operation() == dbCompressed) {
                    if (!prefixKnown) {
                        ConstDataRangeCursor input(raw.header().data(),
                                                   raw.header().data() + raw.header().dataLen());
                        const CompressionHeader header(&input);
                        snappyPrefixFallback = header.compressorId ==
                            static_cast<MessageCompressorId>(MessageCompressor::kSnappy);
                    }
                    MessageCompressorManager manager;
                    reply.message = uassertStatusOK(manager.decompressMessage(raw));
                    raw = Message{};
                } else {
                    reply.message = std::move(raw);
                }
                decodeMicros.increment(decompressTimer.micros());
                uassert(ErrorCodes::UnsupportedFormat,
                        "Replication decoded reply is not OP_MSG",
                        reply.message.operation() == dbMsg);
                if (prefixKnown) {
                    uassert(ErrorCodes::BadValue,
                            "Replication decoded flags differ from control prefix",
                            OpMsg::flags(reply.message) == flags);
                }
                reservation->shrink(reply.message.capacity());
                reply.reservation = reservation;
                Timer validationTimer;
                reply.parsed = std::make_unique<rpc::OpMsgReply>(
                    OpMsg::parseOwnedForReplicationReply(reply.message));
                validateMicros.increment(validationTimer.micros());
                if (!prefixKnown) {
                    if (OpMsg::isFlagSet(reply.message, OpMsg::kMoreToCome)) {
                        // The receiver stopped after this frame because its flags were unknown.
                        // Deliver this reply, then let the fetcher resume synchronously instead of
                        // treating it as a decode fault.
                        reply.endedReadAhead = true;
                    } else {
                        // Published before the reply, so a clean stop() after it is allowed.
                        std::lock_guard lk(mutex);
                        terminalFrameReceived = true;
                    }
                    if (snappyPrefixFallback) {
                        snappyPrefixFallbacks.increment();
                    } else {
                        unsupportedCompressorFallbacks.increment();
                    }
                    LOGV2(
                        11369904,
                        "Receive decode reply required full decoding instead of prefix read-ahead",
                        "reason"_attr =
                            (snappyPrefixFallback ? "snappyPrefix" : "unsupportedCompressor"),
                        "endsReadAhead"_attr = reply.endedReadAhead);
                }
                decoded.increment();
            }
        } catch (const DBException& ex) {
            status = ex.toStatus();
        }
        if (!status.isOK()) {
            reply.parsed.reset();
            reply.message = Message{};
            raw = Message{};
            reply.reservation.reset();
            reservation.reset();
        }
        publish(slot, std::move(status), std::move(reply));
        raw = Message{};
        reservation.reset();
        slot.reset();
        completion.reset();
    }

    void receive() {
        while (!canceled.load() && !failed.load()) {
            {
                std::unique_lock lk(mutex);
                cv.wait(lk, [&] {
                    return canceled.load() || failed.load() ||
                        slots.size() < static_cast<size_t>(gReplReceiveDecodeQueueDepth.load());
                });
            }
            if (canceled.load() || failed.load()) {
                break;
            }
            std::shared_ptr<Slot> slot;
            std::shared_ptr<Completion> completion;
            bool receiving = false;
            try {
                auto reservation = Reservation::acquire(canceled, failed);
                if (!reservation) {
                    break;
                }
                slot = std::make_shared<Slot>();
                {
                    std::lock_guard lk(mutex);
                    slot->seq = nextReceive++;
                    slots.emplace(slot->seq, slot);
                    pendingReplies.increment(1);
                }
                Timer receiveTimer;
                receiving = true;
                auto raw = uassertStatusOK(session->sourceMessage());
                receiving = false;
                recvMicros.increment(receiveTimer.micros());
                uassert(ErrorCodes::BadValue,
                        "Invalid replication wire message size",
                        !raw.empty() && raw.capacity() >= MsgData::MsgDataHeaderSize &&
                            raw.size() >= MsgData::MsgDataHeaderSize &&
                            static_cast<size_t>(raw.size()) <= raw.capacity() &&
                            static_cast<size_t>(raw.size()) <= MaxMessageSizeBytes);
                received.increment();
                uassert(40570,
                        "Response ID did not match the exhaust reply chain",
                        raw.header().getResponseToMsgId() == expectedResponseTo);
                const auto prefix = prefixFlags(raw);
                const auto flags = prefix.flags;
                const auto bytes = raw.capacity() + outputSize(raw);
                uassert(ErrorCodes::BadValue,
                        "Replication message allocation exceeds the receive reservation",
                        bytes <= kReceiveReservation);
                reservation->shrink(bytes);
                expectedResponseTo = raw.header().getId();
                auto task = std::make_unique<DecodeTask>();
                task->raw = std::move(raw);
                task->reservation = std::move(reservation);
                task->flags = flags;
                task->prefixKnown = prefix.known;
                completion = std::make_shared<Completion>();
                completion->state = shared_from_this();
                completion->slot = slot;
                {
                    std::lock_guard lk(mutex);
                    ++tasks;
                    completion->registered = true;
                    terminalFrameReceived = prefix.known && !(flags & OpMsg::kMoreToCome);
                    activeTasks.increment(1);
                }
                task->completion = completion;
                auto injectScheduleFailure = [](const BSONObj& data) {
                    const auto code = data.hasField("errorCode")
                        ? static_cast<ErrorCodes::Error>(data["errorCode"].safeNumberInt())
                        : ErrorCodes::InternalError;
                    uasserted(code, "Injected replication decode schedule failure");
                };
                replReceiveDecodeScheduleThrows.executeIf(
                    injectScheduleFailure,
                    [](const BSONObj& data) { return !data["afterSchedule"].trueValue(); });
                DecodePool::get().schedule([task = std::move(task)](Status status) mutable {
                    auto state = task->completion->state;
                    auto slot = task->completion->slot;
                    state->decode(std::move(slot),
                                  std::move(task->raw),
                                  task->flags,
                                  task->prefixKnown,
                                  std::move(task->reservation),
                                  std::move(task->completion),
                                  std::move(status));
                    hangReplReceiveDecodeAfterTaskCompletion.pauseWhileSet();
                });
                replReceiveDecodeScheduleThrows.executeIf(
                    [&](const BSONObj& data) {
                        if (data["waitForCompletion"].trueValue()) {
                            std::unique_lock lk(mutex);
                            cv.wait(lk, [&] { return slot->done; });
                        }
                        injectScheduleFailure(data);
                    },
                    [](const BSONObj& data) { return data["afterSchedule"].trueValue(); });
                if (!(flags & OpMsg::kMoreToCome)) {
                    break;
                }
            } catch (const DBException& ex) {
                const bool published = slot &&
                    publish(slot,
                            ex.toStatus(),
                            Reply{},
                            receiving && ErrorCodes::isNetworkError(ex.code()));
                // A winning failed publish already stopped reading. Otherwise a worker may have
                // published success before schedule() threw: that result stays valid, but
                // receiving stopped unexpectedly, so the session can't be handed back as healthy.
                if (!published) {
                    {
                        std::lock_guard lk(mutex);
                        // Preserve the receiver error without replacing or recounting the result
                        // already published by the worker.
                        receiverExitStatus = ex.toStatus();
                    }
                    stopReading();
                }
                break;
            }
        }
        std::lock_guard lk(mutex);
        receiverFinished = true;
        cv.notify_all();
    }

    std::shared_ptr<transport::Session> session;
    int expectedResponseTo;
    std::atomic<bool> canceled{false};
    std::atomic<bool> failed{false};
    std::mutex mutex;
    stdx::condition_variable cv;
    std::map<uint64_t, std::shared_ptr<Slot>> slots;
    uint64_t nextReceive = 0;
    uint64_t nextDeliver = 0;
    size_t tasks = 0;
    bool receiverFinished = false;
    // Guarded by 'mutex'. A receiver error that lost the slot publication race.
    Status receiverExitStatus = Status::OK();
    // Guarded by 'mutex'. Only an error reached by the consumer makes delivery permanently fail.
    Status deliveryStatus = Status::OK();
    // Guarded by 'mutex'. Set before dispatch so a fast terminal decoder can safely stop the
    // stream.
    bool terminalFrameReceived = false;
    // Guarded by 'mutex'. Cleared by stop() when the session is handed back to the client.
    bool ownsSession = true;
    // Guarded by 'mutex'. Number of threads currently inside session->end().
    size_t sessionClosers = 0;
};

struct ReceiveDecodePipeline::Receiver {
    explicit Receiver(const std::shared_ptr<State>& state)
        : thread([state] {
              setThreadName("ReplRecv");
              state->receive();
          }) {}
    stdx::thread thread;
};

ReceiveDecodePipeline::ReceiveDecodePipeline(std::shared_ptr<transport::Session> session,
                                             int expectedResponseTo)
    : _state(std::make_shared<State>(std::move(session), expectedResponseTo)) {}

std::shared_ptr<ReceiveDecodePipeline> ReceiveDecodePipeline::make(
    std::shared_ptr<transport::Session> session, int expectedResponseTo) {
    invariant(session);
    // Initialize the checksum dispatch on the caller before parallel parsers can reach its
    // non-atomic first-call cache. The normal authenticated connection is already established.
    static const auto checksumInitialized = [] {
        OpMsg probe;
        probe.body = BSON("ok" << 1);
        auto message = probe.serialize();
        OpMsg::appendChecksum(&message);
        return true;
    }();
    invariant(checksumInitialized);
    DecodePool::get().start();
    auto pipeline = std::shared_ptr<ReceiveDecodePipeline>(
        new ReceiveDecodePipeline(std::move(session), expectedResponseTo));
    pipeline->_receiver = std::make_unique<Receiver>(pipeline->_state);
    return pipeline;
}

ReceiveDecodePipeline::~ReceiveDecodePipeline() {
    stop(true);
}

ReceiveDecodePipeline::Reply ReceiveDecodePipeline::take(
    int expectedResponseTo, const std::function<Status()>& checkInterrupt) {
    Timer waitTimer;
    std::unique_lock lk(_state->mutex);
    uassertStatusOK(_state->deliveryStatus);
    auto ready = [&] {
        auto it = _state->slots.find(_state->nextDeliver);
        return _state->canceled.load() || (it != _state->slots.end() && it->second->done) ||
            (_state->receiverFinished && it == _state->slots.end());
    };
    if (!checkInterrupt) {
        _state->cv.wait(lk, ready);
    } else {
        const auto interval = kInterruptCheckInterval.toSystemDuration();
        while (!_state->cv.wait_for(lk, interval, ready)) {
            // The owner's check may take its own mutex, which cancel() is called under, so never
            // run it while holding the pipeline mutex.
            lk.unlock();
            const auto status = checkInterrupt();
            if (!status.isOK()) {
                orderedWaitMicros.increment(waitTimer.micros());
                uassertStatusOK(status);
            }
            lk.lock();
        }
    }
    orderedWaitMicros.increment(waitTimer.micros());
    uassert(ErrorCodes::CallbackCanceled,
            "Replication receive pipeline canceled",
            !_state->canceled.load());
    auto it = _state->slots.find(_state->nextDeliver);
    if (it == _state->slots.end()) {
        _state->deliveryStatus = _state->receiverExitStatus.isOK()
            ? Status(ErrorCodes::HostUnreachable,
                     "Replication exhaust stream ended without a reply")
            : _state->receiverExitStatus;
        uassertStatusOK(_state->deliveryStatus);
    }
    if (!it->second->status.isOK()) {
        _state->deliveryStatus = it->second->status;
        uassertStatusOK(_state->deliveryStatus);
    }
    if (it->second->reply.message.header().getResponseToMsgId() != expectedResponseTo) {
        _state->deliveryStatus = Status(ErrorCodes::Error(40570),
                                        "Response ID did not match the cursor's exhaust progress");
        const auto status = _state->deliveryStatus;
        lk.unlock();
        _state->stopReading();
        uassertStatusOK(status);
    }
    auto slot = std::move(it->second);
    _state->slots.erase(it);
    ++_state->nextDeliver;
    pendingReplies.decrement(1);
    _state->cv.notify_all();
    lk.unlock();
    delivered.increment();
    return std::move(slot->reply);
}

void ReceiveDecodePipeline::cancel() noexcept {
    _state->cancel();
}

bool ReceiveDecodePipeline::stop(bool abort) {
    if (!_receiver) {
        // Report the handoff result, not a cancel() that arrived after the session was returned.
        return _interruptedAtHandoff;
    }
    uassert(ErrorCodes::IllegalOperation,
            "The replication receiver cannot join itself",
            _receiver->thread.get_id() != std::this_thread::get_id());
    if (abort) {
        cancel();
    } else {
        std::lock_guard lk(_state->mutex);
        uassert(ErrorCodes::IllegalOperation,
                "A healthy replication receive stream can only be stopped after consuming its "
                "terminal frame",
                (_state->terminalFrameReceived && _state->slots.empty()) || _state->failed.load() ||
                    _state->canceled.load());
    }
    _receiver->thread.join();
    _receiver.reset();
    {
        std::unique_lock lk(_state->mutex);
        _state->cv.wait(lk, [&] { return _state->tasks == 0; });
        pendingReplies.decrement(_state->slots.size());
        _state->slots.clear();
    }
    // The receiver has exited and every decode task has published its result; a worker may still
    // be unwinding locals, but it can no longer close the session. Hand the session back only
    // after any concurrent close has finished, and make every later cancel() a no-op for it.
    return _interruptedAtHandoff = _state->detachSession();
}

void ReceiveDecodePipeline::shutdownPool() {
    DecodePool::get().shutdown();
}

}  // namespace mongo::repl
