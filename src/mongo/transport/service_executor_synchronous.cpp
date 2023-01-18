/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/transport/service_executor_synchronous.h"

#include <array>
#include <deque>
#include <fmt/format.h>

#include "mongo/base/status.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_synchronous_synchronization.h"
#include "mongo/transport/service_executor_synchronous_thread.h"
#include "mongo/transport/session.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scoped_unlock.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/thread_safety_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

namespace mongo::transport {
namespace {

using namespace fmt::literals;

// Central switch for debug instrumentation.
constexpr int kLocalDbg = 5;

Status notRunningStatus() {
    return {ErrorCodes::ShutdownInProgress, "Executor is not running"};
}

size_t cores() {
    static const auto numCores = ProcessInfo::getNumAvailableCores();
    return numCores;
}

enum class BucketId {
    none,
    starting,
    ready,
    leased,
    stopping,
};

StringData bucketName(BucketId b) {
    static constexpr std::array names{
        "none"_sd,
        "starting"_sd,
        "ready"_sd,
        "leased"_sd,
        "stopping"_sd,
    };
    return names[static_cast<size_t>(b)];
}

std::ostream& toString(std::ostream& os, BucketId b) {
    return os << bucketName(b);
}

class ManagedBuckets {
public:
    class Member;
    using Bucket = std::list<std::shared_ptr<Member>>;

    /** Intrusive base class for objects that want to be in a `ManagedBucket`. */
    class Member {
    public:
        virtual ~Member() {
            invariant(!bucket, "Destroyed while held by a bucket");
        }

    private:
        friend class ManagedBuckets;
        Bucket* bucket = nullptr;
        typename Bucket::iterator iterator;
    };

    struct BucketSizes {
        size_t starting;
        size_t ready;
        size_t leased;
        size_t stopping;
    };

    class SyncAccess {
    public:
        explicit SyncAccess(const ManagedBuckets* sourcePtr) : _sourcePtr{sourcePtr} {}

        BucketSizes bucketSizes() const {
            return {_src()._starting.size(),
                    _src()._ready.size(),
                    _src()._leased.size(),
                    _src()._stopping.size()};
        }

        size_t totalSize() const {
            auto bs = bucketSizes();
            return bs.starting + bs.ready + bs.leased + bs.stopping;
        }

        /** Move `w` from whatever bucket it's in now (if any) to the `toId` bucket. */
        void move(const std::shared_ptr<Member>& w, BucketId toId) {
            _moveLocked(w, toId);
        }

        /** Move the back of `fromId` and into `toId`. */
        std::shared_ptr<Member> popMove(BucketId fromId, BucketId toId) {
            auto from = _src()._bucket(fromId);
            if (from->empty())
                return nullptr;
            auto w = from->back();
            _moveLocked(w, toId);
            return w;
        }

    private:
        void _moveLocked(const std::shared_ptr<Member>& w, BucketId destId) {
            Bucket* destPtr = _src()._bucket(destId);
            Member& bm = *w;
            if (destPtr) {
                if (bm.bucket) {
                    destPtr->splice(destPtr->end(), *bm.bucket, bm.iterator);
                } else {
                    destPtr->push_back(w);
                    bm.iterator = std::prev(destPtr->end());
                }
            } else {
                if (bm.bucket)
                    bm.bucket->erase(bm.iterator);
            }
            bm.bucket = destPtr;
        }

        const ManagedBuckets& _src() const {
            return *_sourcePtr;
        }

        ManagedBuckets& _src() {
            return *const_cast<ManagedBuckets*>(_sourcePtr);
        }

        const ManagedBuckets* _sourcePtr;
        Synchronization::Lock _lk{_sourcePtr->_sync.acquireLock()};
    };

    explicit ManagedBuckets(std::shared_ptr<SyncDomain> domain)
        : _sync{"buckets", std::move(domain)} {}

    std::string toString() const {
        auto bs = sync().bucketSizes();
        return "[starting={}/ready={}/leased={}/stopping={}]"_format(
            bs.starting, bs.ready, bs.leased, bs.stopping);
    }

    SyncAccess sync() const {
        return SyncAccess{this};
    }

private:
    Bucket* _bucket(BucketId id) {
        switch (id) {
            case BucketId::none:
                return nullptr;
            case BucketId::starting:
                return &_starting;
            case BucketId::ready:
                return &_ready;
            case BucketId::leased:
                return &_leased;
            case BucketId::stopping:
                return &_stopping;
        }
        MONGO_UNREACHABLE;
    }

    mutable Synchronization _sync;
    Bucket _starting;
    Bucket _ready;
    Bucket _leased;
    Bucket _stopping;
};

class Worker : public std::enable_shared_from_this<Worker>, public ManagedBuckets::Member {
public:
    class LeaseToken;

    class Env {
    public:
        virtual ~Env() = default;

        virtual StringData name() const = 0;

        virtual bool isRunning() const = 0;

        virtual size_t getRunningThreads() const = 0;

        /** May throw ShutdownInProgress. */
        virtual void onReadyThread(const std::shared_ptr<Worker>& w) = 0;

        /** May throw ShutdownInProgress. */
        virtual void onReleasedThread(const std::shared_ptr<Worker>& w) = 0;

        virtual void onEndThread(const std::shared_ptr<Worker>& w) = 0;

        virtual void yield() const = 0;

        /**
         * The worker's Synchronization object will participate in an
         * env-specified SyncDomain if there is one, or it can be nullptr.
         */
        virtual std::shared_ptr<SyncDomain> syncDomain() = 0;
    };

    Worker(std::unique_ptr<Env> env, uint64_t id)
        : _env{std::move(env)}, _id{id}, _sync{"worker{}"_format(_id), _env->syncDomain()} {
        LOGV2_DEBUG(7015101, kLocalDbg, "Worker ctor", "w"_attr = _id);
    }

    ~Worker() {
        LOGV2_DEBUG(7015102, kLocalDbg, "Worker dtor", "w"_attr = _id);
    }

    /**
     * The worker's executor. It pins a reference to a Worker thread on which it
     * can schedule tasks. There is one per Worker, and when it dies, it moves its
     * worker thread into the executor's ready list.
     * From there, the executor can destroy it or lease it out again as needed.
     */
    std::unique_ptr<LeaseToken> makeLease();

    void shutdown() {
        auto lk = _sync.acquireLock();
        shutdownLocked();
    }

    void shutdownLocked() {
        _state = State::stopped;
        _sync.notifyAll();
    }

    void run() {
        ScopeGuard epilogue = [&] { _env->onEndThread(shared_from_this()); };
        try {
            _env->onReadyThread(shared_from_this());
        } catch (const ExceptionFor<ErrorCodes::ShutdownInProgress>&) {
            shutdown();
        }
        auto lk = _sync.acquireLock();
        while (true) {
            _sync.wait(lk, [&] { return _state != State::idle; });
            switch (_state) {
                case State::idle:
                    break;
                case State::leased:
                    _serveLease(lk);
                    break;
                case State::released:
                    _state = State::idle;
                    try {
                        _env->onReleasedThread(shared_from_this());
                    } catch (const ExceptionFor<ErrorCodes::ShutdownInProgress>&) {
                        _state = State::stopped;
                    }
                    break;
                case State::stopped:
                    _serveLease(lk, notRunningStatus());
                    return;
            }
        }
    }

    uint64_t getId() const {
        return _id;
    }

private:
    enum class State {
        idle,
        leased,
        released,
        stopped,
    };

    friend StringData toString(State s) {
        return std::array{
            "idle"_sd,
            "leased"_sd,
            "released"_sd,
            "stopped"_sd,
        }[static_cast<int>(s)];
    }

    void _onDestroyLeaseToken() {
        auto lk = _sync.acquireLock();
        if (_state != State::leased) {
            invariant(_state == State::stopped);
            return;
        }
        _state = State::released;
        _sync.notifyOne();
    }

    void _schedule(ServiceExecutor::Task task) {
        LOGV2_DEBUG(7015106, kLocalDbg, "Schedule", "w"_attr = getId());
        if (MONGO_unlikely(!_env->isRunning())) {
            task(notRunningStatus());
            return;
        }
        auto lk = _sync.acquireLock();
        _tasks.push_back(std::move(task));
        _sync.notifyOne();
    }

    /**
     * Schedules task immediately, on the assumption that The task will block to
     * receive the next message and we don't mind blocking on this dedicated
     * worker thread.
     */
    void _runOnDataAvailable(const std::shared_ptr<Session>& session, ServiceExecutor::Task task) {
        invariant(session);
        _schedule(std::move(task));
    }

    /**
     * Runs for the duration of a worker lease, calling queued tasks with the
     * specified `execStatus`, watching for state changes. If this worker
     * becomes un-leased, any queued tasks are still invoked, and the function
     * returns.
     */
    void _serveLease(Synchronization::Lock& lk, Status execStatus = Status::OK()) {
        while (true) {
            _sync.wait(lk, [&] { return _state != State::leased || !_tasks.empty(); });
            if (_tasks.empty())
                return;
            auto t = std::move(_tasks.front());
            _tasks.pop_front();
            ScopedUnlock unlock{lk};
            LOGV2_DEBUG(
                7015100, kLocalDbg, "Run task", "w"_attr = getId(), "executor"_attr = _env->name());
            t(execStatus);
            t = nullptr;
        }
    }

    void _yield() const {
        _env->yield();
    }

    const std::unique_ptr<Env> _env;
    const uint64_t _id;
    Synchronization _sync;
    std::deque<ServiceExecutor::Task> _tasks;
    State _state = State::idle;
};

class Worker::LeaseToken : public ServiceExecutor::Executor {
public:
    explicit LeaseToken(std::shared_ptr<Worker> worker) : _w{std::move(worker)} {
        LOGV2_DEBUG(7015107, kLocalDbg, "LeaseToken ctor", "w"_attr = _w->getId());
    }

    ~LeaseToken() {
        LOGV2_DEBUG(7015108, kLocalDbg, "LeaseToken dtor", "w"_attr = _w->getId());
        _w->_onDestroyLeaseToken();
    }

    void schedule(Task task) override {
        _w->_schedule(std::move(task));
    }

    void runOnDataAvailable(const std::shared_ptr<Session>& session, Task task) override {
        _w->_runOnDataAvailable(session, std::move(task));
    }

    void yieldPointReached() const override {
        _w->_yield();
    }

private:
    std::shared_ptr<Worker> _w;
};

std::unique_ptr<Worker::LeaseToken> Worker::makeLease() {
    auto lk = _sync.acquireLock();
    if (_state != State::idle)
        return nullptr;
    _state = State::leased;
    return std::make_unique<LeaseToken>(shared_from_this());
}

class DedicatedThreadExecutorPool
    : public std::enable_shared_from_this<DedicatedThreadExecutorPool> {
private:
    size_t _availableThreads(const ManagedBuckets::BucketSizes& bs) const {
        return bs.starting + bs.ready;
    }

    /** If we have less than reserve, we need more. */
    bool _needsMoreWorkers(Synchronization::Lock& lk, const ManagedBuckets::BucketSizes& bs) const {
        return _availableThreads(bs) < _waitingForLease + _reservedThreads;
    }

    /** We keep a few threads alive to reduce spawning. */
    bool _needsFewerWorkers(Synchronization::Lock& lk,
                            const ManagedBuckets::BucketSizes& bs) const {
        return _availableThreads(bs) >= _waitingForLease + _reservedThreads + _maxIdleThreads;
    }

    /** Called when a new worker has started, becoming ready. */
    void _onReadyThread(const std::shared_ptr<Worker>& w) {
        LOGV2_DEBUG(7015109, kLocalDbg, "A new Worker has become ready", "w"_attr = w->getId());
        auto lk = _sync.acquireLock();
        if (!_isRunning.loadRelaxed())
            iasserted(notRunningStatus());
        _workers.sync().move(_asMember(w), BucketId::ready);
        _sync.notifyAll();
    }

    /**
     * Worker `w` has been released from a lease.
     * A decision is made here about whether to keep the worker or discard it.
     */
    void _onReleasedThread(const std::shared_ptr<Worker>& w) {
        LOGV2_DEBUG(7015110, kLocalDbg, "Worker released from lease", "w"_attr = w->getId());
        auto lk = _sync.acquireLock();
        if (!_isRunning.loadRelaxed())
            iasserted(notRunningStatus());
        auto workersSync = _workers.sync();
        auto bs = workersSync.bucketSizes();
        if (_needsFewerWorkers(lk, bs)) {
            w->shutdownLocked();
            workersSync.move(w, BucketId::none);
            _updateLazyShouldYield(workersSync.bucketSizes());
        } else {
            workersSync.move(w, BucketId::ready);
        }
        _sync.notifyAll();
    }

    /** Remove a dying worker from the buckets as its run() concludes. */
    void _onEndThread(const std::shared_ptr<Worker>& w) {
        LOGV2_DEBUG(7015111, kLocalDbg, "Worker end", "w"_attr = w->getId());
        auto lk = _sync.acquireLock();
        _workers.sync().move(w, BucketId::none);
        _sync.notifyAll();
    }

public:
    explicit DedicatedThreadExecutorPool(std::string name,
                                         size_t reservedThreads,
                                         size_t maxIdleThreads,
                                         std::string statLabel)
        : _name{std::move(name)},
          _reservedThreads{reservedThreads},
          _maxIdleThreads{maxIdleThreads},
          _statLabel{std::move(statLabel)},
          _workers{_sync.domain()} {}

    Status start() try {
        auto lk = _sync.acquireLock();
        _isRunning.store(true);
        _sync.notifyAll();
        _spawnEnoughWorkers(lk);
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    size_t getRunningThreads() const {
        return _workers.sync().totalSize();
    }

    void appendStats(BSONObjBuilder* bob) const {
        auto bs = _workers.sync().bucketSizes();
        int threads = bs.starting + bs.ready + bs.leased + bs.stopping;
        int clients = bs.leased;
        BSONObjBuilder{bob->subobjStart(_statLabel)}
            .append("threadsRunning", threads)
            .append("clientsInTotal", clients)
            .append("clientsRunning", clients)
            .append("clientsWaitingForData", 0);
    }

    /** Lease a worker, spawning new workers as necessary. */
    std::unique_ptr<ServiceExecutor::Executor> makeTaskRunner() {
        LOGV2_DEBUG(7015112, kLocalDbg, "makeTaskRunner");
        auto lk = _sync.acquireLock();
        ++_waitingForLease;
        ScopeGuard restoreWaitingForLease = [&] { --_waitingForLease; };
        while (MONGO_likely(_isRunning.loadRelaxed())) {
            try {
                _spawnEnoughWorkers(lk);
            } catch (const DBException& ex) {
                LOGV2_DEBUG(7015103, 3, "Nonfatal spawn failure", "error"_attr = ex);
            }

            auto [w, bs] = _nextWorker(lk);
            if (!w)
                continue;
            _updateLazyShouldYield(bs);

            // `w->makeLease` locks `w`, so release subordinate mutex.
            ScopedUnlock unlockThis{lk};
            if (auto lease = w->makeLease())
                return lease;
            // Retry. Lease failed as w is no longer idle.
            // Another requester must have gotten it first.
        }
        iasserted(notRunningStatus());
    }

    bool isRunning() const {
        return _isRunning.loadRelaxed();
    }

    Status shutdown(Milliseconds timeout) {
        LOGV2_DEBUG(22982, 3, "Shutting down executor", "name"_attr = _name);
        auto lk = _sync.acquireLock();
        _isRunning.store(false);
        {
            ScopedUnlock unlock{lk};
            for (auto b : {BucketId::starting, BucketId::ready, BucketId::leased})
                while (auto w = _workers.sync().popMove(b, BucketId::stopping))
                    _asWorker(w)->shutdown();
        }
        _sync.notifyAll();
        if (!_sync.waitFor(lk, timeout, [&] {
                LOGV2_DEBUG(7015113, kLocalDbg, "waitForDrain");
                return _workers.sync().totalSize() == 0;
            })) {
            return Status(
                ErrorCodes::Error::ExceededTimeLimit,
                "Executor {} couldn't shutdown within {}."_format(_name, timeout.toString()));
        }
        return Status::OK();
    }

private:
    static std::shared_ptr<ManagedBuckets::Member> _asMember(std::shared_ptr<Worker> w) {
        return std::dynamic_pointer_cast<ManagedBuckets::Member>(std::move(w));
    }
    static std::shared_ptr<Worker> _asWorker(std::shared_ptr<ManagedBuckets::Member> w) {
        return std::dynamic_pointer_cast<Worker>(std::move(w));
    }

    /** The environment that `_makeWorker` imbues into its products. */
    class WorkerEnv : public Worker::Env {
    public:
        explicit WorkerEnv(std::shared_ptr<DedicatedThreadExecutorPool> source)
            : _source{std::move(source)} {}
        StringData name() const override {
            return _source->_name;
        }
        bool isRunning() const override {
            return _source->isRunning();
        }
        size_t getRunningThreads() const override {
            return _source->getRunningThreads();
        }
        void onReadyThread(const std::shared_ptr<Worker>& w) override {
            _source->_onReadyThread(w);
        }
        void onReleasedThread(const std::shared_ptr<Worker>& w) override {
            _source->_onReleasedThread(w);
        }
        void onEndThread(const std::shared_ptr<Worker>& w) override {
            _source->_onEndThread(w);
        }
        std::shared_ptr<SyncDomain> syncDomain() override {
            return _source->_sync.domain();
        }
        void yield() const override {
            return _source->_yield();
        }

    private:
        std::shared_ptr<DedicatedThreadExecutorPool> _source;
    };

    /**
     * Only the leased workers are competing for cores.
     * This metric has never accounted for other threads competing with this
     * executor's threads. Even among the ServiceExecutors, Synchronous and
     * Reserved can be simultanenously active.
     */
    void _updateLazyShouldYield(ManagedBuckets::BucketSizes bs) {
        bool newValue = bs.leased > cores();
        if (_lazyShouldYield.loadRelaxed() != newValue)
            _lazyShouldYield.store(newValue);
    }

    void _yield() const {
        if (_lazyShouldYield.loadRelaxed())
            stdx::this_thread::yield();
    }

    void _spawnEnoughWorkers(Synchronization::Lock& lk) {
        while (_needsMoreWorkers(lk, _workers.sync().bucketSizes())) {
            LOGV2_DEBUG(7015114,
                        3,
                        "Starting a new thread",
                        "executor"_attr = _name,
                        "waitingForLease"_attr = _waitingForLease,
                        "buckets"_attr = _workers.toString());
            ScopedUnlock unlock{lk};
            auto w = _makeWorker();
            _workers.sync().move(w, BucketId::starting);
            try {
                iassert(launchServiceWorkerThread([w]() mutable { std::move(w)->run(); }));
            } catch (...) {
                _workers.sync().move(w, BucketId::none);
                throw;
            }
        }
    }

    std::tuple<std::shared_ptr<Worker>, ManagedBuckets::BucketSizes> _nextWorker(
        Synchronization::Lock& lk) {
        for (;;) {
            _sync.wait(lk, [&] {
                auto bs = _workers.sync().bucketSizes();
                return !_isRunning.loadRelaxed() || bs.ready || !bs.starting;
            });
            if (!_isRunning.loadRelaxed())
                iasserted(notRunningStatus());
            auto workersSync = _workers.sync();
            auto bs = workersSync.bucketSizes();
            if (bs.ready)
                return {_asWorker(workersSync.popMove(BucketId::ready, BucketId::leased)), bs};
            if (!bs.starting)
                iasserted({ErrorCodes::InternalError, "No ready workers"});
        }
    }

    std::shared_ptr<Worker> _makeWorker() {
        return std::make_shared<Worker>(std::make_unique<WorkerEnv>(shared_from_this()),
                                        _workerIdCounter.fetchAndAdd(1));
    }

    const std::string _name;
    const size_t _reservedThreads;
    const size_t _maxIdleThreads;
    const std::string _statLabel;
    AtomicWord<uint64_t> _workerIdCounter{1};
    mutable Synchronization _sync{"executor"};  ///< Root of its domain
    AtomicWord<bool> _isRunning;
    size_t _waitingForLease = 0;
    ManagedBuckets _workers;

    /** Updated in a relaxed manner when a Worker lease is created or destroyed. */
    AtomicWord<bool> _lazyShouldYield;
};

}  // namespace

// =======================
// ServiceExecutorSyncBase
// =======================

class ServiceExecutorSyncBase::Impl : public DedicatedThreadExecutorPool {
public:
    using DedicatedThreadExecutorPool::DedicatedThreadExecutorPool;
};

ServiceExecutorSyncBase::ServiceExecutorSyncBase(std::string name,
                                                 size_t reservedThreads,
                                                 size_t maxIdleThreads,
                                                 std::string statLabel)
    : _impl{std::make_shared<Impl>(
          std::move(name), reservedThreads, maxIdleThreads, std::move(statLabel))} {}

ServiceExecutorSyncBase::~ServiceExecutorSyncBase() = default;

Status ServiceExecutorSyncBase::start() {
    return _impl->start();
}

Status ServiceExecutorSyncBase::shutdown(Milliseconds timeout) {
    return _impl->shutdown(timeout);
}

size_t ServiceExecutorSyncBase::getRunningThreads() const {
    return _impl->getRunningThreads();
}

void ServiceExecutorSyncBase::appendStats(BSONObjBuilder* bob) const {
    _impl->appendStats(bob);
}

std::unique_ptr<ServiceExecutor::Executor> ServiceExecutorSyncBase::makeTaskRunner() {
    return _impl->makeTaskRunner();
}

// ==========================
// ServiceExecutorSynchronous
// ==========================

namespace {
const auto serviceExecutorSynchronousDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<ServiceExecutorSynchronous>>();

const ServiceContext::ConstructorActionRegisterer serviceExecutorSynchronousRegisterer{
    "ServiceExecutorSynchronous", [](ServiceContext* ctx) {
        serviceExecutorSynchronousDecoration(ctx) = std::make_unique<ServiceExecutorSynchronous>();
    }};
}  // namespace

ServiceExecutorSynchronous::ServiceExecutorSynchronous()
    : ServiceExecutorSyncBase{"passthrough", defaultReserved, cores(), "passthrough"} {}

ServiceExecutorSynchronous* ServiceExecutorSynchronous::get(ServiceContext* ctx) {
    auto& ref = serviceExecutorSynchronousDecoration(ctx);
    invariant(ref);
    return ref.get();
}

// =======================
// ServiceExecutorReserved
// =======================

namespace {
const auto serviceExecutorReservedDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<ServiceExecutorReserved>>();

const ServiceContext::ConstructorActionRegisterer serviceExecutorReservedRegisterer{
    "ServiceExecutorReserved", [](ServiceContext* ctx) {
        auto threads = serverGlobalParams.reservedAdminThreads;
        if (threads)
            serviceExecutorReservedDecoration(ctx) =
                std::make_unique<ServiceExecutorReserved>("admin connections", threads, cores());
    }};
}  // namespace

ServiceExecutorReserved::ServiceExecutorReserved(std::string name,
                                                 size_t reservedThreads,
                                                 size_t maxIdleThreads)
    : ServiceExecutorSyncBase{std::move(name), reservedThreads, maxIdleThreads, "reserved"} {}

ServiceExecutorReserved* ServiceExecutorReserved::get(ServiceContext* ctx) {
    return serviceExecutorReservedDecoration(ctx).get();
}

}  // namespace mongo::transport
