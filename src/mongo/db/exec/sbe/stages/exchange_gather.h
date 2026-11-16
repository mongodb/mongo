// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/stages/exchange_worker_pool.h"
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/platform/pause.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <vector>

namespace mongo {
namespace sbe {
/**
 * Gathers values produced by multi-threaded clones of the single input streams into a single output
 * stream. At runtime the 'input' stage, which provides M slots specified as a vector of slot
 * vectors 'inputVals', is multiplied into N parallel threads that work together to produce the data
 * that this stage must process. The stage itself returns those M slots, picked cyclically from each
 * thread. Each thread materializes the rows it produces into rows that own their values, and the
 * queue of those rows is emptied one by one into '_currentRow', which the parent stage(s) consume
 * by means of the '_outValueAccessors'. Because a thread's rows never point back into its own
 * slots, it is put back to work as soon as its batch has been picked up off the queue.
 *
 * This is a binding reflector, meaning that only the 'inputVals' slots are visible to stages
 * higher in the tree.
 *
 * Debug string representation:
 *
 *   exchange_gather dop=<dop> [<input slots>]
 */
class ExchangeGather final : public PlanStage {
public:
    ExchangeGather(std::unique_ptr<PlanStage> input,
                   value::SlotVector inputVals,
                   size_t dop,
                   PlanYieldPolicySBE* yieldPolicy,
                   PlanNodeId planNodeId,
                   bool participateInTrialRunTracking = true);

    std::unique_ptr<PlanStage> clone() const final;

    void prepare(CompileCtx& ctx) final;
    value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) final;
    void open(bool reOpen) final;
    PlanState getNext() final;
    void close() final;

    std::unique_ptr<PlanStageStats> getStats(bool includeDebugInfo) const final;
    const SpecificStats* getSpecificStats() const final;
    void doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                      DebugPrintInfo& debugPrintInfo) const final;
    size_t estimateCompileTimeSize() const final;

protected:
    bool shouldOptimizeSaveState(size_t idx) const final {
        return true;
    }

    void doSaveState() override;
    void doRestoreState() override;
    void doDetachFromOperationContext() override;
    void doAttachToOperationContext(OperationContext* opCtx) override;

private:
    /**
     * The indexes of the slots exposed by this stage on behalf of its input stages.
     */
    const value::SlotVector _inputVals;
    /**
     * The level of parallelism this stage should try to achieve.
     */
    const size_t _dop;

    // Global lock mode held by the originating operation, captured on the consumer thread in
    // open(). The producer threads take the same mode on their own OperationContexts.
    LockMode _globalLockMode = MODE_IS;

    /**
     * Class representing one producer running in its own dedicated thread.
     */
    struct Producer {
        Producer(ExchangeWorkerPool::Handle worker,
                 std::unique_ptr<PlanStage> stage,
                 CompileCtx ctx)
            : worker(std::move(worker)), plan(std::move(stage)), compileCtx(std::move(ctx)) {
            plan->attachToOperationContext(this->worker->opCtxHolder.get());
        }

        // Gives the leased worker back to the pool (if one was ever leased) before any other member
        // is torn down, since the worker may still be running a job that touches 'plan'/
        // 'opCtxHolder'/'client'. Returning a worker waits for that job to finish.
        ~Producer() {
            // Safety measure in case close() hasn't been called. Deallocate the plan so that any
            // outstanding cursor is closed, then release the thread with its Client.
            worker->waitIdle();
            plan.reset();
            worker.reset();
        }

        // Hands 'newJob' to this producer's worker thread and returns immediately. At most one job
        // may be outstanding at a time; callers are responsible for not calling submit() again
        // before the previous job has signaled its own completion (e.g. via the Promise/Future it
        // captured).
        void submit(unique_function<void()> newJob) {
            worker->submit(std::move(newJob));
        }

        // Like submit(), but returns a Future fulfilled with the result of 'func'.
        template <typename Func>
        auto submitAsPromise(Func&& func) {
            return worker->submitAsPromise(std::forward<Func>(func));
        }

        // Blocks until this producer's thread has no job outstanding, i.e. until whatever was
        // last submit()'d (if anything) has finished running.
        bool waitIdle(Microseconds timeout) {
            return worker->waitIdle(timeout);
        }

        // The worker thread leased from the process-wide pool; empty for the single-threaded
        // fallback producer.
        ExchangeWorkerPool::Handle worker;

        // The plan subtree that should be executed inside the parallel thread.
        std::unique_ptr<PlanStage> plan;
        // The thread-safe copy of the compile context needed to compile the plan.
        CompileCtx compileCtx;
        // One accessor per slot in '_inputVals', in the same order. Allow a quick access to the
        // accessors exposed by the stages living in the plan subtree that must be made available to
        // parent stages.
        std::vector<value::SlotAccessor*> accessors;
        // Signals that the subtree has finished iterating over the dedicated data partition.
        bool isEof = false;

        // Stats counters.
        Microseconds totalWallTime{0};
        Microseconds maxWallTime{0};
        Microseconds minWallTime{0};
        size_t numExecutions{0};
        size_t numProducedDocs{0};
    };

    // The outcome of one producer's batch of getNext() calls: either the PlanState the last one
    // returned, or the exception it threw.
    //
    // 'rows' holds every row the producer produced during the batch, in production order. Each row
    // fully owns its values, so nothing in a ReadyResult points back into the producer's own
    // accessors; the producer is therefore free to run again as soon as its result has been popped
    // off the queue. 'rows' is empty only when the producer's very first getNext() of the batch did
    // not return ADVANCED.
    struct ReadyResult {
        size_t idx = 0;
        PlanState state = PlanState::IS_EOF;
        std::exception_ptr exception;
        std::vector<value::MaterializedRow> rows;
    };

    /**
     * Multi-producer/single-consumer queue of 'ReadyResult's, into which each producer pushes its
     * own result once getNext() has returned (or thrown) on its thread, and out of which the single
     * driving thread pops them in the order producers actually finished.
     *
     * push() and tryPop() are lock free: they only ever touch atomics in a fixed-size circular
     * buffer. This is safe without any capacity check because each producer may have at most one
     * outstanding job at a time (see Producer::submit()), so at most one entry per producer can be
     * live in the queue; sizing the buffer to hold that many entries makes overflow impossible.
     * The capacity bound is only a logical guarantee, though: to make a producer's reuse of a slot
     * happen after the consumer's move out of it, push() acquires the 'ready = false' that
     * tryPop() releases before writing.
     *
     * A mutex and a condition variable are still kept for the blocking pop(), but they are only
     * touched on the slow path where the consumer actually has to go to sleep. Producers grab the
     * mutex to notify only when '_waiters' tells them a consumer is (about to be) sleeping.
     *
     * The queue also owns a free list of drained 'ReadyResult's, so that the row vector a producer
     * fills in is normally one the consumer has already handed back rather than a freshly grown
     * one. allocate()/recycle() are the two ends of that list and may be called from any thread.
     */
    class ReadyQueue {
    public:
        // Sizes the queue to hold 'maxOutstanding' entries, i.e. one per producer. Must be called
        // before any push()/pop(), and never concurrently with them.
        void resize(size_t maxOutstanding) {
            // Round the capacity up to a power of two so that the index wraparound below is a
            // cheap mask instead of a division.
            size_t capacity = 1;
            while (capacity < maxOutstanding) {
                capacity *= 2;
            }
            _slots = std::vector<Slot>(capacity);
            _mask = capacity - 1;
            _writeTicket.store(0);
            _readTicket = 0;

            // At most one entry per producer can be outstanding at a time, plus the one the
            // consumer is holding on to, so the free list never usefully grows beyond that.
            _maxRecycled = maxOutstanding + 1;
            _recycled.clear();
            _recycled.reserve(_maxRecycled);
        }

        // Remove any ReadyResult already in the queue.
        void clear() {
            boost::optional<ReadyResult> result;
            while ((result = tryPop())) {
                recycle(std::move(*result));
            }
        }

        // Hands out a 'ReadyResult' for a producer to fill in, reusing one the consumer has already
        // handed back if there is any. Note that a reused result still carries the rows of the
        // batch it was used for last: they are there to be refilled in place, so the producer must
        // treat 'rows' as scratch space rather than as the rows it has produced so far, and
        // truncate it to what it actually filled in. Only 'exception' is guaranteed to be clear.
        // Callable from any thread.
        ReadyResult allocate() {
            std::lock_guard lk(_recycleMutex);
            if (_recycled.empty()) {
                return ReadyResult{};
            }
            ReadyResult result = std::move(_recycled.back());
            _recycled.pop_back();
            return result;
        }

        // Takes a drained 'ReadyResult' back for reuse by a later allocate(). Callable from any
        // thread. Not calling it is always safe: the only consequence is a missed reuse.
        void recycle(ReadyResult result) {
            result.exception = nullptr;

            std::lock_guard lk(_recycleMutex);
            if (_recycled.size() < _maxRecycled) {
                _recycled.emplace_back(std::move(result));
            }
        }

        // Lock free. Callable from any thread, including the producers. Note that the result is
        // inherently racy: a producer observing 'true' only knows the consumer is waiting at some
        // point during the call, as another producer may push concurrently.
        bool waiting() const {
            return _waiters.loadRelaxed() != 0;
        }

        // Lock free. Callable from any producer thread.
        void push(ReadyResult result) {
            Slot& slot = _slots[_writeTicket.fetchAndAddRelaxed(1) & _mask];
            // Because of the capacity bound the consumer has already popped the previous entry, so
            // this only spins while its store is still becoming visible; a long spin would mean the
            // one-outstanding-job invariant was broken.
            while (slot.ready.load()) {
                MONGO_YIELD_CORE_FOR_SMT();
            }
            slot.result = std::move(result);
            // Publishes 'slot.result' to the consumer. Sequentially consistent (rather than just
            // release) so that it cannot be reordered with the '_waiters' load below: together with
            // the matching ordering in pop(), that is what guarantees we never fail to wake a
            // consumer that is about to sleep.
            slot.ready.store(true);

            if (_waiters.load() != 0) {
                // Take the mutex around the notification so we cannot signal in between the
                // consumer's predicate check and its wait().
                std::lock_guard lk(_mutex);
                _cv.notify_one();
            }
        }

        // Lock free. Only callable from the single consumer thread.
        boost::optional<ReadyResult> tryPop() {
            Slot& slot = _slots[_readTicket & _mask];
            if (!slot.ready.load()) {
                return boost::none;
            }
            boost::optional<ReadyResult> result{std::move(slot.result)};
            // Releases the slot back to the producers (pairs with the load in push()); must come
            // after the read above.
            slot.ready.store(false);
            ++_readTicket;
            return result;
        }

        // Blocks until an entry is available or 'timeout' has elapsed, in which case it returns
        // boost::none. Only callable from the single consumer thread.
        boost::optional<ReadyResult> pop(Microseconds timeout) {
            // Announce ourselves before looking at the queue, so that a producer publishing an
            // entry concurrently either sees '_waiters' and notifies us, or is already visible to
            // the tryPop() below.
            _waiters.fetchAndAddRelaxed(1);

            // Spin on the queue for a while before going to sleep. A producer that saw '_waiters'
            // above cuts its batch short and pushes as soon as its current getNext() returns, which
            // is usually a matter of microseconds; sleeping through that window would cost a
            // futex round trip and the latency of being woken back up. The spin is bounded, so a
            // genuinely slow producer still ends up on the condition variable below rather than
            // burning this core.
            boost::optional<ReadyResult> result;
            for (size_t attempt = 0; attempt < kPopSpinAttempts; attempt++) {
                result = tryPop();
                if (result) {
                    _waiters.fetchAndSubtractRelaxed(1);
                    return result;
                }
                MONGO_YIELD_CORE_FOR_SMT();
            }

            {
                std::unique_lock lk(_mutex);
                // The predicate makes any notification lost between the spin above and this wait()
                // harmless.
                // If the wait times out, 'result' is empty, as the last predicate evaluation
                // failed.
                _cv.wait_for(lk, timeout.toSystemDuration(), [&] {
                    return (result = tryPop()).has_value();
                });
            }
            _waiters.fetchAndSubtractRelaxed(1);
            return result;
        }

    private:
        // How many times pop() checks the queue before it gives up and sleeps. Chosen to cover the
        // time a producer needs to notice '_waiters' and hand its batch over, which is bounded by
        // one of its getNext() calls, without spinning long enough to matter when it does not.
        static constexpr size_t kPopSpinAttempts = 256;

        struct Slot {
            Slot() = default;
            // Only used to grow the vector in resize(), which happens before any concurrent
            // access, so copying the (always false) flag is fine.
            Slot(const Slot&) {}

            mongo::Atomic<bool> ready{false};
            ReadyResult result{};
        };

        std::vector<Slot> _slots;
        size_t _mask = 0;

        mongo::Atomic<uint64_t> _writeTicket{0};
        // Only ever touched by the consumer thread, hence not atomic.
        uint64_t _readTicket = 0;

        mongo::Atomic<uint64_t> _waiters{0};
        std::mutex _mutex;
        stdx::condition_variable _cv;

        // Drained 'ReadyResult's, kept around so their row vectors can be filled in again instead
        // of regrown from scratch. Guarded by its own mutex, kept separate from '_mutex' so that
        // handing a result out never contends with a consumer going to sleep. The critical sections
        // are a move and a pop_back, and are entered once per batch.
        std::mutex _recycleMutex;
        std::vector<ReadyResult> _recycled;
        size_t _maxRecycled = 0;
    };

    // The rows of a ReadyResult that still have to be handed to our parent stage, one getNext()
    // call at a time.
    struct PendingResult {
        std::vector<value::MaterializedRow> rows;
        // Index of the next entry of 'rows' to expose.
        size_t pos = 0;

        bool done() const {
            return pos == rows.size();
        }
    };

    /**
     * Moves the next row of '_pending' into '_currentRow' and returns ADVANCED. Must only be called
     * while '_pending' is not done().
     */
    PlanState servePending();

    // Blocks until all the jobs tracked by 'futures' have completed, and propagates the first
    // failure. The producers need to take the same locks held by this thread, so they can be
    // queued behind a conflicting request that is in turn waiting for this thread's locks: to avoid
    // a deadlock, if 'canYield' is set this thread periodically checks for interrupts and yields
    // while waiting.
    void waitForProducers(std::vector<Future<void>> futures, bool canYield);

    // Marks the operation running on every producer's thread as killed with 'killCode'.
    void killProducers(ErrorCodes::Error killCode);

    // Whether the producers have been started since the last open().
    bool _started = false;
    // The queue holding the rows produced by each parallel thread.
    ReadyQueue _readyQueue;
    // The block of rows being served to the parent stage.
    PendingResult _pending;
    // The row currently exposed to our parent stage, moved out of 'PendingResult::rows' by
    // servePending(). Owns its values, so it stays valid no matter what the producers do next.
    value::MaterializedRow _currentRow;
    // One accessor per slot in '_inputVals', each a view of the corresponding entry of
    // '_currentRow'.
    std::vector<value::MaterializedSingleRowAccessor> _outValueAccessors;
    std::vector<std::unique_ptr<Producer>> _producers;

    // Stats on the execution
    size_t _numIdleCycles{0};
    Microseconds _totalIdleTime;
};
}  // namespace sbe
}  // namespace mongo
