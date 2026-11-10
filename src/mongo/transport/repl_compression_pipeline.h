// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/functional.h"
#include "mongo/util/static_immortal.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include <boost/optional.hpp>

namespace mongo::transport {

/**
 * Called when the head batch finishes compressing while its getMore may be parked in awaitData.
 * The db layer installs it (this library can't depend on db/query); it releases only that
 * operation's wait. opCtxTag is the OperationContext passed to registerActiveOpCtx(). Null in unit
 * tests, in which case nothing is woken.
 */
using ReplPipelineAwaitDataWakeFn = void (*)(void* opCtxTag);
void setReplPipelineAwaitDataWakeFn(ReplPipelineAwaitDataWakeFn fn);

/**
 * Called on the connection thread from registerActiveOpCtx(), before the getMore runs, so that its
 * awaitData wait installs a waker the hook above can trigger.
 */
using ReplPipelineArmOpCtxFn = void (*)(void* opCtxTag);
void setReplPipelineArmOpCtxFn(ReplPipelineArmOpCtxFn fn);

/**
 * One response in the pipeline. Filled in on the connection thread, compressed by a worker that
 * only sees copies of raw/compressorId, and sent by the connection thread in seq order.
 */
struct ReplPipelineBatch {
    uint64_t seq = 0;
    MessageCompressorId compressorId = 0;

    // Uncompressed response. Dropped once compression finishes.
    Message raw;

    // May be the uncompressed message if compressing would exceed the wire limit; send as-is.
    Message compressed;

    // Non-OK means the session must be torn down.
    Status status = Status::OK();

    bool compressedDone = false;

    // rawSize is the reply's wire size (metrics only). Until compression finishes the batch holds
    // rawReservedBytes = max(raw capacity, rawSize) in the global budget, since getMore replies can
    // keep a buffer much larger than the reply. Afterwards it holds
    // accountedBytes = max(result capacity, wireSize), or 0 on failure.
    // compressedSize is only used for metrics.
    size_t rawSize = 0;
    size_t rawReservedBytes = 0;
    size_t compressedSize = 0;
    size_t accountedBytes = 0;

    // Traffic-recorder request taken from the opCtx at submit time; recorded together with the
    // compressed reply just before it is sent.
    boost::optional<Message> trafficRecorderRequest;
};

/**
 * Compression pool shared by all sessions. Created on first use; its size comes from a
 * startup-only parameter and never changes. Not restarted after shutdown().
 */
class ReplCompressionThreadPool {
public:
    static ReplCompressionThreadPool& get();

    /** Create and start the pool on first call; never restart after shutdown(). */
    void ensureStarted();

    /**
     * Runs `task` on a worker. The task gets a non-OK status if the pool is shut down and must
     * still publish a result; in that case it runs inline on the calling thread. There is no
     * fallback if a worker thread can't be created: stdx::thread terminates the process instead.
     * May throw before the task is queued, in which case the task is destroyed without running.
     */
    void schedule(unique_function<void(Status)> task);

    /**
     * Stops admission, waits for schedule() calls admitted before shutdown began, then joins the
     * workers, which run every task already queued. Concurrent callers wait for the same shutdown.
     * Tasks rejected because shutdown already started run inline on their caller and may still be
     * running when this returns.
     *
     * Must not be called from a task running on this pool: it would wait for that task to finish.
     */
    void shutdown();

    bool isShutdown() const {
        return _shutdown.load();
    }

private:
    template <typename T>
    friend class ::mongo::StaticImmortal;
    friend class ReplCompressionThreadPoolTest;

    ReplCompressionThreadPool() = default;

    static size_t _resolvePoolSize();

    std::mutex _mutex;
    stdx::condition_variable _cvShutdown;
    std::shared_ptr<ThreadPool> _pool;
    AtomicWord<bool> _shutdown{false};
    bool _shutdownComplete = false;
    size_t _activeSchedulers = 0;
};

/**
 * Per-session pipeline. Workers hold a weak_ptr and drop their result if the session is gone.
 * Batches may finish compressing out of order but are always sent in seq order.
 */
class ReplyCompressionPipeline : public std::enable_shared_from_this<ReplyCompressionPipeline> {
public:
    ~ReplyCompressionPipeline();

    static std::shared_ptr<ReplyCompressionPipeline> make() {
        return std::shared_ptr<ReplyCompressionPipeline>(new ReplyCompressionPipeline());
    }

    /**
     * Queues `raw` for compression and takes ownership of it. Returns false without touching `raw`
     * if the pipeline is disabled or shut down, the byte cap is reached, or the window is full;
     * the caller then compresses inline after draining older batches. Doesn't wait for room in the
     * window, but takes _mutex briefly.
     *
     * If allocating the batch throws, the pipeline is left unchanged and `raw` is untouched. Once
     * the batch is registered, a task that is destroyed without running completes the batch with
     * a failed status, which the sender reports like a compression failure. If scheduling throws
     * a DBException this still returns true; any other exception propagates, with the batch
     * already completed as failed.
     */
    bool trySubmit(Message& raw,
                   MessageCompressorId compressorId,
                   boost::optional<Message> trafficRecorderRequest = boost::none);

    /**
     * Waits for the next batch in seq order and hands it out. Returns false if the pipeline is shut
     * down. Throws if that batch failed to compress. The caller should call recordBatchSent() once
     * the batch has been written to the session.
     */
    bool takeNextReady(Message* out, boost::optional<Message>* outRecorderRequest = nullptr);

    /** Like takeNextReady(), but returns false right away if the next batch isn't ready yet. */
    bool tryTakeNextReady(Message* out, boost::optional<Message>* outRecorderRequest = nullptr);

    /** Number of in-flight (submitted, not yet sent) batches. */
    size_t inFlight() const;

    /** True if there is at least one submitted-but-not-sent batch. */
    bool hasPending() const;

    /**
     * Max in-flight batches per session. 0 if the pipeline is disabled or the global byte cap is
     * exceeded.
     */
    static size_t effectiveQueueDepth();

    /** Bytes currently reserved in the global budget. For tests and diagnostics. */
    static long long globalPendingBytes();

    /** Counts a batch from takeNextReady()/tryTakeNextReady() as written to the session. */
    static void recordBatchSent();

    /**
     * Wakes the sender and drops unsent batches. Safe to call from any thread. Tasks that haven't
     * run yet skip compression and release their own reservation.
     */
    void shutdown();

    bool isShutdown() const;

    /**
     * Sets the getMore the connection thread is about to run as the wake target, and wakes it right
     * away if the head batch is already done. The caller must call clearActiveOpCtx() before that
     * OperationContext is destroyed. The wake hook runs under _mutex, so it can't race with clear.
     */
    void registerActiveOpCtx(void* opCtxTag);
    void clearActiveOpCtx();

private:
    class TaskReservation;

    ReplyCompressionPipeline() = default;

    // Schedules the compression task for an already registered and reserved batch. May throw if
    // the task can't be queued; `reservation` then publishes a failed result for `seq`.
    static void _scheduleCompression(TaskReservation reservation,
                                     uint64_t seq,
                                     Message rawForWorker,
                                     MessageCompressorId cid,
                                     size_t rawReservedBytes);

    // Publishes a worker's result and swaps its reservation from rawReservedBytes to
    // accountedBytes. If the batch is gone, releases rawReservedBytes instead.
    void _onCompressed(
        uint64_t seq, Message compressed, Status status, size_t wireSize, size_t rawReservedBytes);

    // Takes the head batch (must be compressedDone), advances, releases its reservation and
    // unlocks `lk`. Throws if the batch failed.
    bool _extractReadyBatchLocked(std::unique_lock<std::mutex>& lk,
                                  Message* out,
                                  boost::optional<Message>* outRecorderRequest);

    mutable std::mutex _mutex;
    stdx::condition_variable _cvReady;  // sender waits here

    // Batches from submit until sent, keyed by seq.
    std::map<uint64_t, std::shared_ptr<ReplPipelineBatch>> _batches;

    uint64_t _nextSeq = 0;
    uint64_t _nextSeqToSend = 0;
    bool _shutdown = false;

    // At most one wake per head batch. Reset when a batch is taken or a new opCtx registers.
    bool _wakeSignalled = false;

    // Current wake target, or null. Guarded by _mutex.
    void* _activeOpCtx = nullptr;
};

}  // namespace mongo::transport
