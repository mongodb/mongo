// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/stages/exchange_gather.h"

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/compile_ctx.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

namespace {
// A producer keeps reading ahead (buffering the rows it produces) for as long as the
// getNext() calls stay below this threshold.
constexpr Microseconds kReadAheadThreshold{Microseconds{500}};
// A producer keeps reading ahead (buffering the rows it produces) for as long as the
// consumer thread has data to process and getNext() calls stay below this threshold.
constexpr Microseconds kReadAheadMaxThreshold{Milliseconds{5}};
// Maximum time the consumer thread can wait for a producer before waking up and checking whether
// the query has been interrupted or if it's time to yield.
constexpr Microseconds kMaxWaitBeforeYield{Milliseconds{5}};

// Takes the global lock on a producer's opCtx with the mode held by the originating operation. If
// the originating operation holds no global lock, none is taken.
boost::optional<Lock::GlobalLock> acquireGlobalLock(OperationContext* opCtx, LockMode mode) {
    if (!opCtx || mode == MODE_NONE) {
        return boost::none;
    }
    return boost::optional<Lock::GlobalLock>{boost::in_place_init, opCtx, mode};
}
}  // namespace

ExchangeGather::ExchangeGather(std::unique_ptr<PlanStage> input,
                               value::SlotVector inputVals,
                               size_t dop,
                               PlanYieldPolicySBE* yieldPolicy,
                               PlanNodeId planNodeId,
                               bool participateInTrialRunTracking)
    : PlanStage("exchange_gather"sv, yieldPolicy, planNodeId, participateInTrialRunTracking),
      _inputVals{std::move(inputVals)},
      _dop(dop) {
    _children.emplace_back(std::move(input));

    uassert(13436500, "Expected DOP to be >= 2", _dop >= 2);

    // All of the slots listed in '_inputVals' must be unique.
    value::SlotSet dupCheck;
    for (auto& slot : _inputVals) {
        auto inserted = dupCheck.insert(slot).second;
        uassert(13436501, str::stream() << "duplicate input field: " << slot, inserted);
    }
}

std::unique_ptr<PlanStage> ExchangeGather::clone() const {
    return std::make_unique<ExchangeGather>(_children[0]->clone(),
                                            _inputVals,
                                            _dop,
                                            _yieldPolicy,
                                            _commonStats.nodeId,
                                            participateInTrialRunTracking());
}

void ExchangeGather::prepare(CompileCtx& ctx) {
    // Prepare the branch used in single-threaded mode, even if we are not going to use it because
    // we can use the parallel threads; saveState()/restoreState() will automatically propagate the
    // attach/detach operations to every children during yielding, and they would fail if
    // preparation hasn't been done.
    _children[0]->prepare(ctx);

    if (!ctx.correlated.empty()) {
        // If we are part of a plan branch that needs to access correlated slots produced by another
        // branch that is running in serial mode, we don't have a safe way of accessing those slot,
        // so fallback to single-threaded plan.
        return;
    }
    // Lease _dop threads from a global pool. The number of returned thread handle can be lower than
    // the requested _dop according to the number of available worker threads (global counter set
    // via server parameter setting the max number of worker threads) and the availability of WT
    // tickets.
    std::vector<ExchangeWorkerPool::Handle> workers =
        ExchangeWorkerPool::get(cc().getService()->getServiceContext()).acquire(_opCtx, _dop);
    if (workers.size() < 2) {
        // Not enough worker threads available at this time, so fallback to single threaded mode.
        return;
    }

    std::vector<std::unique_ptr<PlanStage>> clones =
        _children[0]->cloneForParallelExecution(ctx, workers.size());
    if (clones.size() < 2) {
        // Some stages don't support parallel execution, so fallback to single threaded mode.
        return;
    }
    // Prepare N copies of the child stage subtree.
    for (auto& t : clones) {
        auto worker = std::move(workers.back());
        workers.pop_back();
        _producers.push_back(std::make_unique<Producer>(
            std::move(worker), std::move(t), ctx.makeCopyForParallelUse()));
    }

    for (auto& producer : _producers) {
        // Forward the setting regarding collection of timing info.
        producer->plan->markShouldCollectTimingInfo(_commonStats.executionTime.precision);
        // TODO: SERVER-137780 Ensure that compileCtx.mca is initialized properly so that
        // ScanStages can reach the collection
        producer->compileCtx.mca = ctx.mca;
        // TODO: SERVER-139245 move it be done on the separate thread; if prepare() ends up
        // acquiring resources we must be ready to yield to avoid deadlocks here too.
        producer->plan->prepare(producer->compileCtx);
        // Resolve, for each exposed slot, the accessor the corresponding slot of each
        // producer's plan. This must happen after preparing each producer above, since a
        // producer's getAccessor() is only guaranteed to return a valid accessor once it has
        // been prepared.
        for (auto inputSlot : _inputVals) {
            producer->accessors.emplace_back(
                producer->plan->getAccessor(producer->compileCtx, inputSlot));
        }
    }
    // Each producer can have at most one getNext() result outstanding at a time, so the queue
    // never needs to hold more than one entry per producer.
    _readyQueue.resize(_producers.size());

    // Every row a producer hands over is materialized into a MaterializedRow that owns its
    // values, and the row we currently expose lives in '_currentRow'. So a plain view of
    // '_currentRow' is all our parent ever needs.
    _currentRow.resize(_inputVals.size());
    _outValueAccessors.reserve(_inputVals.size());
    for (size_t slotIdx = 0; slotIdx < _inputVals.size(); slotIdx++) {
        _outValueAccessors.emplace_back(_currentRow, slotIdx);
    }
}

value::SlotAccessor* ExchangeGather::getAccessor(CompileCtx& ctx, value::SlotId slot) {
    if (_producers.empty()) {
        // The single-threaded fallback never materializes anything: getNext() just forwards to
        // its only children, so we can hand out that children's accessors directly and stay a
        // pure pass-through.
        return _children[0]->getAccessor(ctx, slot);
    }
    // If the slot is one of our outputs, locate its position in the materialized row.
    for (size_t idx = 0; idx < _inputVals.size(); idx++) {
        if (_inputVals[idx] == slot) {
            return &_outValueAccessors[idx];
        }
    }

    return ctx.getAccessor(slot);
}

void ExchangeGather::open(bool reOpen) {
    auto optTimer(getOptTimer(_opCtx));

    _commonStats.opens++;
    if (!reOpen) {
        _numIdleCycles = 0;
        _totalIdleTime = Microseconds{0};
    }

    if (_producers.empty()) {
        // Single threaded plan.
        _children[0]->open(reOpen);
        return;
    }

    // A reopen can happen without a close() after a partial consumption: producers may still be
    // running a getNext() job, and the queue may hold results of the previous execution. Wait for
    // every outstanding job, then discard what they produced, before restarting the producers.
    for (auto& producer : _producers) {
        while (!producer->waitIdle(kMaxWaitBeforeYield)) {
            checkForInterruptAndYield(_opCtx);
        }
    }
    _readyQueue.clear();

    _started = false;
    _pending = PendingResult{};

    // Producers must take the same global lock mode that the originating operation holds. Read it
    // here, on the consumer thread, as the locker must not be accessed concurrently from the
    // producers (it can be released and reacquired while yielding).
    _globalLockMode = shard_role_details::getLocker(_opCtx)->getLockMode(resourceIdGlobal);

    // Open each producer's input stage on its own dedicated thread, then block until all of them
    // have finished before returning.
    std::vector<Future<void>> futures;
    futures.reserve(_producers.size());
    for (auto& producer : _producers) {
        producer->isEof = false;

        // Capture the producer by raw pointer: the objects are heap-allocated with stable
        // addresses and '_producers' outlives this call, since we wait for every future below.
        futures.emplace_back(producer->submitAsPromise(
            [producerPtr = producer.get(), reOpen, lockMode = _globalLockMode] {
                auto lock = acquireGlobalLock(producerPtr->worker->opCtxHolder.get(), lockMode);
                producerPtr->plan->open(reOpen);
                producerPtr->plan->saveState();
            }));
    }

    // Wait for all the open() calls to finish, propagating the first exception if any.
    waitForProducers(std::move(futures), /*canYield=*/true);
}

void ExchangeGather::killProducers(ErrorCodes::Error killCode) {
    for (auto& producer : _producers) {
        // The producer thread may still be running on this opCtx, so the owning Client must be
        // locked to kill it.
        auto* workerOpCtx = producer->worker->opCtxHolder.get();
        ClientLock lk(workerOpCtx->getClient());
        workerOpCtx->markKilled(killCode);
    }
}

void ExchangeGather::waitForProducers(std::vector<Future<void>> futures, bool canYield) {
    if (!_opCtx) {
        // If we don't have an OperationContext connected we can't be interrupted and we cannot
        // yield to avoid deadlocking other threads, so just wait for the close() calls to finish.
        whenAllSucceed(std::move(futures)).get();
        return;
    }

    Notification<void> done;
    Status result = Status::OK();
    // The callback only touches locals that outlive it, as we never return before 'done' is set.
    whenAllSucceed(std::move(futures)).unsafeToInlineFuture().getAsync([&](Status status) {
        result = std::move(status);
        done.set();
    });

    boost::optional<Status> interruption;
    while (!done) {
        if (!canYield) {
            // This is used while closing the plan, which must not be interrupted. If the producers
            // don't finish right away they may be stuck acquiring the locks held by this thread, so
            // release them while waiting, and take them back afterwards.
            const bool ready = _opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&] {
                return done.waitFor(_opCtx, duration_cast<Milliseconds>(kMaxWaitBeforeYield));
            });
            if (!ready) {
                if (shard_role_details::getLocker(_opCtx)->canSaveLockState()) {
                    auto yielded = yieldTransactionResourcesFromOperationContext(_opCtx);
                    done.get();
                    UninterruptibleLockGuard noInterrupt(_opCtx);  // NOLINT
                    try {
                        restoreTransactionResourcesToOperationContext(_opCtx, std::move(yielded));
                    } catch (const DBException&) {
                        // The plan is being closed and nothing will use the resources again: if
                        // they can't be restored (e.g. the collection was dropped in the
                        // meantime) they are released, and there is nothing else to do.
                    }
                } else {
                    done.get();
                }
            }
            break;
        }
        // Wake up periodically, so that we can check for interrupts and release the locks if
        // needed. Never leave while the producers are still running.
        try {
            if (done.waitFor(_opCtx, duration_cast<Milliseconds>(kMaxWaitBeforeYield))) {
                break;
            }
            checkForInterruptAndYield(_opCtx);
        } catch (const DBException& ex) {
            interruption = ex.toStatus();
            // Make the producers stop early, then wait for them to be done.
            killProducers(interruption->code());
            done.get();
            break;
        }
    }
    uassertStatusOK(interruption ? *interruption : result);
}

void ExchangeGather::doSaveState() {}
void ExchangeGather::doRestoreState() {}
void ExchangeGather::doDetachFromOperationContext() {}
void ExchangeGather::doAttachToOperationContext(OperationContext* opCtx) {}

PlanState ExchangeGather::getNext() {
    auto optTimer(getOptTimer(_opCtx));

    if (_producers.empty()) {
        // Single threaded plan.
        return trackPlanState(_children[0]->getNext());
    }

    checkForInterruptAndYield(_opCtx);

    // Keep draining the batch we last popped; the producer that filled it is already running again.
    if (!_pending.done()) {
        return trackPlanState(servePending());
    }

    auto asyncStart = [&](Producer* producer, size_t idx) {
        producer->submit([this, producer, idx]() {
            // Obtain the storage for the result from the queue, possibly reusing one from a
            // previous execution.
            ReadyResult ready = _readyQueue.allocate();
            ready.idx = idx;
            try {
                auto lock = acquireGlobalLock(producer->worker->opCtxHolder.get(), _globalLockMode);
                producer->plan->restoreState();
                Timer timer;
                // Every row produced, including the last one, is materialized into a row that owns
                // its values, so the producer keeps nothing live in its own accessors once the
                // batch is over and can be restarted the moment the consumer pops its result.
                // Batching several rows into a single ReadyResult amortizes the queue/handoff cost
                // with the consumer; after we have been running for a while, if the consumer thread
                // is sitting idle, stop and hand over what we have.
                size_t pos = 0;
                while (true) {
                    ready.state = producer->plan->getNext();
                    if (ready.state != PlanState::ADVANCED) {
                        break;
                    }

                    // The storage could come from a previous execution, and already have some rows
                    // to reuse. Insert a new row only if the space was exhausted.
                    if (pos == ready.rows.size()) {
                        ready.rows.emplace_back(producer->accessors.size());
                    }
                    value::MaterializedRow& row = ready.rows[pos++];
                    for (size_t i = 0; i < producer->accessors.size(); i++) {
                        row.reset(i, producer->accessors[i]->copyOrMoveValue());
                    }

                    // Ensure we amortize the time spent in synchronization by running for a minimum
                    // time; after that, keep running until we detect that the main thread is idle
                    // waiting for us, or if we reach the max time.
                    // REVISIT: should we be looking at the number of accumulated rows?
                    if (timer.elapsed() >= kReadAheadThreshold &&
                        (_readyQueue.waiting() || timer.elapsed() >= kReadAheadMaxThreshold)) {
                        break;
                    }
                }
                // Trim the unused rows.
                ready.rows.resize(pos);
                Microseconds elapsed = timer.elapsed();
                producer->totalWallTime += elapsed;
                producer->maxWallTime = std::max(producer->maxWallTime, elapsed);
                producer->minWallTime = producer->minWallTime.count() == 0
                    ? elapsed
                    : std::min(producer->minWallTime, elapsed);
                producer->numExecutions++;
                producer->numProducedDocs += ready.rows.size();
                producer->plan->saveState();
            } catch (...) {
                ready.exception = std::current_exception();
            }
            _readyQueue.push(std::move(ready));
        });
    };

    // On the very first call, start every producer. From then on each of them is restarted as soon
    // as its result is popped below, so there is nothing to kick off here. Each producer's
    // OperationContext was already attached in open(), so it is safe to run on any pool
    // thread here.
    if (!_started) {
        for (size_t idx = 0; idx < _producers.size(); idx++) {
            asyncStart(_producers[idx].get(), idx);
        }
        _started = true;
    }

    auto state = PlanState::IS_EOF;
    // Loop until all of them have signaled they are at the end of their partition.
    while (std::any_of(
        _producers.begin(), _producers.end(), [](auto& producer) { return !producer->isEof; })) {
        // Check if a producer has already placed a result in the queue.
        boost::optional<ReadyResult> ready = _readyQueue.tryPop();
        while (!ready) {
            Timer timer;
            // Wake up periodically, so that we can check for user-triggered operation interrupts.
            ready = _readyQueue.pop(kMaxWaitBeforeYield);
            _numIdleCycles++;
            _totalIdleTime += timer.elapsed();
            // Check if we have to interrupt or yield.
            checkForInterruptAndYield(_opCtx);
        }

        if (ready->exception) {
            std::rethrow_exception(ready->exception);
        }
        if (ready->state == PlanState::IS_EOF) {
            _producers[ready->idx]->isEof = true;
        } else {
            // The producer owns nothing that we are about to read, so put it straight back to work
            // instead of waiting until we have drained the rows it just handed us.
            asyncStart(_producers[ready->idx].get(), ready->idx);
        }

        if (ready->rows.empty()) {
            // Nothing at all from this producer; check another one.
            _readyQueue.recycle(std::move(*ready));
            continue;
        }
        // This producer has found data that can be exposed to our parent stages. Swap rather than
        // move, so that the batch we just finished draining goes back to the producers along with
        // the capacity of its row vector; recycle() is what frees the values it still holds.
        _pending.rows.swap(ready->rows);
        _pending.pos = 0;
        _readyQueue.recycle(std::move(*ready));
        state = servePending();
        break;
    }

    return trackPlanState(state);
}

PlanState ExchangeGather::servePending() {
    tassert(13436502, "no more rows to serve from queued producers", !_pending.done());

    // A single move of the whole row: '_outValueAccessors' are views of '_currentRow', so there is
    // no per-slot work to do here. The row we are replacing is swapped back into '_pending.rows',
    // where it is freed along with the rest of the batch.
    _currentRow = std::move(_pending.rows[_pending.pos++]);
    return PlanState::ADVANCED;
}

void ExchangeGather::close() {
    auto optTimer(getOptTimer(_opCtx));

    trackClose();

    if (_producers.empty()) {
        // Single threaded plan.
        _children[0]->close();
        return;
    }

    // Abort all the running threads, then wait for them to return.
    if (_opCtx && _opCtx->isKillPending()) {
        killProducers(_opCtx->getKillStatus());
    }
    for (auto& producer : _producers) {
        // If we don't have an OperationContext, don't use a timeout, we would not be able to yield.
        while (!producer->waitIdle(_opCtx ? kMaxWaitBeforeYield : Microseconds::max())) {
            try {
                checkForInterruptAndYield(_opCtx);
            } catch (const DBException&) {
                // The plan is being closed and nothing will use the resources again: we just needed
                // to ensure we had no locks open.
            }
        }
    }

    // Close each producer's input stage on its own dedicated thread, then block until all of them
    // have finished before returning.
    std::vector<Future<void>> futures;
    futures.reserve(_producers.size());
    for (auto& producer : _producers) {
        // Capture the producer by raw pointer: the objects are heap-allocated with stable
        // addresses and '_producers' outlives this call, since we wait for every future below.
        futures.emplace_back(producer->submitAsPromise([producerPtr = producer.get(),
                                                        lockMode = _globalLockMode,
                                                        detachOpCtx = (_opCtx == nullptr)] {
            // Don't lock the OperationContext, there is no request for close() to have a lock
            // available. If the closing operation requires access to the database, it will do the
            // locking as needed. Also, if the main query has been detached, detach also this
            // parallel query so that it doesn't attempt to access the storage.
            if (detachOpCtx) {
                producerPtr->plan->detachFromOperationContext();
            }
            producerPtr->plan->close();
        }));
    }

    // Wait for all the close() calls to finish, propagating the first exception if any.
    waitForProducers(std::move(futures), /*canYield=*/false);

    _readyQueue.clear();
    _started = false;
    _pending = PendingResult{};
    // Drop the values of the last row served, keeping the row sized so that '_outValueAccessors'
    // stay valid; they now read Nothing.
    _currentRow.resize(_inputVals.size());
}

std::unique_ptr<PlanStageStats> ExchangeGather::getStats(bool includeDebugInfo) const {
    auto ret = std::make_unique<PlanStageStats>(_commonStats);

    if (includeDebugInfo) {
        BSONObjBuilder bob;
        bob.appendNumber("actualDop",
                         _producers.empty() ? 1 : static_cast<long long>(_producers.size()));
        bob.appendNumber("dop", static_cast<long long>(_dop));
        bob.append("inputSlots", _inputVals.begin(), _inputVals.end());
        bob.appendNumber("numIdleCycles", static_cast<long long>(_numIdleCycles));
        if (_commonStats.executionTime.precision == QueryExecTimerPrecision::kMillis) {
            bob.appendNumber("totalIdleTimeMillis", durationCount<Milliseconds>(_totalIdleTime));
        } else if (_commonStats.executionTime.precision == QueryExecTimerPrecision::kNanos) {
            bob.appendNumber("totalIdleTimeMillis", durationCount<Milliseconds>(_totalIdleTime));
            bob.appendNumber("totalIdleTimeMicros", durationCount<Microseconds>(_totalIdleTime));
        }

        if (!_producers.empty()) {
            BSONArrayBuilder bab;
            for (auto& producer : _producers) {
                BSONObjBuilder bob;
                bob.appendNumber("numExecutions", static_cast<long long>(producer->numExecutions));
                bob.appendNumber("numProducedDocs",
                                 static_cast<long long>(producer->numProducedDocs));
                if (_commonStats.executionTime.precision == QueryExecTimerPrecision::kMillis) {
                    bob.appendNumber("minWallTimeMicros",
                                     durationCount<Microseconds>(producer->minWallTime));
                    bob.appendNumber("maxWallTimeMicros",
                                     durationCount<Microseconds>(producer->maxWallTime));
                    bob.appendNumber("totalWallTimeMillis",
                                     durationCount<Milliseconds>(producer->totalWallTime));
                } else if (_commonStats.executionTime.precision ==
                           QueryExecTimerPrecision::kNanos) {
                    bob.appendNumber("minWallTimeMicros",
                                     durationCount<Microseconds>(producer->minWallTime));
                    bob.appendNumber("maxWallTimeMicros",
                                     durationCount<Microseconds>(producer->maxWallTime));
                    bob.appendNumber("totalWallTimeMillis",
                                     durationCount<Milliseconds>(producer->totalWallTime));
                    bob.appendNumber("totalWallTimeMicros",
                                     durationCount<Microseconds>(producer->totalWallTime));
                }
                bab.append(bob.obj());
            }
            bob.appendArray("producersStat", bab.arr());
        }
        ret->debugInfo = bob.obj();
    }

    if (_producers.empty()) {
        ret->children.emplace_back(_children[0]->getStats(includeDebugInfo));
    } else {
        for (auto& producer : _producers) {
            ret->children.emplace_back(producer->plan->getStats(includeDebugInfo));
        }
    }
    return ret;
}

const SpecificStats* ExchangeGather::getSpecificStats() const {
    return nullptr;
}

void ExchangeGather::doDebugPrint(std::vector<DebugPrinter::Block>& ret,
                                  DebugPrintInfo& debugPrintInfo) const {
    DebugPrinter::addKeyword(ret, "dop");
    ret.emplace_back(std::to_string(_dop));

    ret.emplace_back(DebugPrinter::Block("[`"));
    for (size_t idx = 0; idx < _inputVals.size(); idx++) {
        if (idx) {
            ret.emplace_back(DebugPrinter::Block("`,"));
        }
        DebugPrinter::addIdentifier(ret, _inputVals[idx]);
    }
    ret.emplace_back(DebugPrinter::Block("`]"));

    ret.emplace_back(DebugPrinter::Block::cmdIncIndent);
    DebugPrinter::addBlocks(ret, _children[0]->debugPrint(debugPrintInfo));
    ret.emplace_back(DebugPrinter::Block::cmdDecIndent);
}

size_t ExchangeGather::estimateCompileTimeSize() const {
    size_t size = sizeof(*this);
    size += size_estimator::estimate(_children);
    size += size_estimator::estimate(_inputVals);
    for (auto& producer : _producers) {
        size += size_estimator::estimate(producer->plan);
    }
    return size;
}

}  // namespace mongo::sbe
