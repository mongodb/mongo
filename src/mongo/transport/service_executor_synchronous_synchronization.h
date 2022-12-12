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

#include <algorithm>
#include <boost/optional.hpp>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/stacktrace.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

// Augment synchronization operations with verbose logging, deadlock detection, backtraces?
#ifndef MONGO_SERVICE_EXECUTOR_SYNCHRONOUS_LOCK_DEBUGGING_ENABLE
#define MONGO_SERVICE_EXECUTOR_SYNCHRONOUS_LOCK_DEBUGGING_ENABLE 0
#endif

namespace mongo::transport {

/**
 * All Synchronization objects that interact with each other will share a SyncDomain.
 * This is a small book keeping structure that has an overall awareness of
 * the state of all objects, to instrument their interactions.
 * Its features are only used in instrumented builds.
 */
class SyncDomain {
public:
    SyncDomain() = default;

    void afterLock(std::string name) {
        stdx::unique_lock lk{_mutex};
        auto& rec = _threads[_tid()];
        rec.holds.push_back(ThreadRecord::Hold{*std::exchange(rec.wants, {}), _trace()});
    }

    void onUnlock(const std::string& name) {
        stdx::unique_lock lk{_mutex};
        auto iter = _threads.find(_tid());
        invariant(iter != _threads.end());
        auto&& holds = iter->second.holds;
        auto holdIter =
            std::find_if(holds.begin(), holds.end(), [&](auto&& el) { return el.name == name; });
        invariant(holdIter != holds.end());
        holds.erase(holdIter);
    }

    void beforeLock(std::string name) {
        stdx::unique_lock lk{_mutex};
        auto iter = _threads.find(_tid());
        if (iter == _threads.end())
            iter =
                _threads.insert({_tid(), ThreadRecord{_tid(), std::string{getThreadName()}}}).first;
        auto& myRec = iter->second;
        myRec.wants = std::move(name);
        std::vector<const ThreadRecord*> seekerChain{&myRec};
        _findCycle(seekerChain);
    }

    BSONObj alsoHolding() const {
        stdx::unique_lock lk{_mutex};
        auto iter = _threads.find(_tid());
        if (iter == _threads.end())
            return {};
        return iter->second.toBSON();
    }

private:
    struct ThreadRecord {
        struct Hold {
            explicit Hold(std::string name, std::string backtrace)
                : name{std::move(name)}, backtrace{std::move(backtrace)} {}
            std::string name;
            std::string backtrace;
        };
        stdx::thread::id tid;
        std::string threadName;
        boost::optional<std::string> wants;
        std::vector<Hold> holds;

        BSONObj toBSON() const {
            BSONObjBuilder b;
            b.append("threadName", threadName);
            b.append("wants", wants.value_or(""));
            {
                BSONArrayBuilder holdsArr{b.subarrayStart("holds")};
                for (auto&& h : holds)
                    holdsArr.append(h.name);
            }
            return b.obj();
        }
    };

    std::string _trace() const {
        std::string backtrace;
        StringStackTraceSink traceSink{backtrace};
        printStackTrace(traceSink);
        return backtrace;
    }

    void _findCycle(std::vector<const ThreadRecord*>& seekers) const;

    stdx::thread::id _tid() const {
        return stdx::this_thread::get_id();
    }

    mutable stdx::mutex _mutex;  // NOLINT
    std::map<stdx::thread::id, ThreadRecord> _threads;
};

/**
 * Abstraction to instrument synchronization behavior. This is basically a
 * mutex and an associated condition variable, but can be built with lots of
 * debug instrumentation.
 */
class Synchronization {
private:
    static constexpr bool _instrumented = MONGO_SERVICE_EXECUTOR_SYNCHRONOUS_LOCK_DEBUGGING_ENABLE;

    class InstrumentedMutex {
    private:
        struct LockRecord {
            stdx::thread::id id;
            std::string name;
        };

        class HeldBy {
        public:
            void set() {
                stdx::lock_guard lk{_mutex};
                _lockRecord = LockRecord{stdx::this_thread::get_id(), std::string{getThreadName()}};
            }

            void reset() {
                stdx::lock_guard lk{_mutex};
                _lockRecord.reset();
            }

            boost::optional<LockRecord> get() const {
                stdx::lock_guard lk{_mutex};
                return _lockRecord;
            }

            std::string toString() const {
                stdx::lock_guard lk{_mutex};
                if (_lockRecord)
                    return _lockRecord->name;
                return {};
            }

        private:
            mutable stdx::mutex _mutex;  // NOLINT
            boost::optional<LockRecord> _lockRecord;
        };

    public:
        explicit InstrumentedMutex(Synchronization* source) : _source{source} {}

        void lock() {
            HeldBy& h = _heldBy;
            _source->_domain->beforeLock(_source->_name);
            _mutex.lock();
            _source->_domain->afterLock(_source->_name);
            h.set();
        }

        void unlock() {
            _mutex.unlock();
            _source->_domain->onUnlock(_source->_name);
            _heldBy.reset();
        }

    private:
        Synchronization* _source;
        Mutex _mutex;
        HeldBy _heldBy;
    };

    class BareMutex {
    public:
        explicit BareMutex(Synchronization*) {}
        void lock() {
            _mutex.lock();
        }
        void unlock() {
            _mutex.unlock();
        }

    private:
        Mutex _mutex;
    };

    using MyMutex = std::conditional_t<_instrumented, InstrumentedMutex, BareMutex>;

    MyMutex _initMutex() {
        return MyMutex{this};
    }

    std::shared_ptr<SyncDomain> _initRootDomain() {
        if constexpr (_instrumented)
            return std::make_shared<SyncDomain>();
        else
            return nullptr;
    }

public:
    using Lock = stdx::unique_lock<MyMutex>;

    explicit Synchronization(std::string name)
        : Synchronization{std::move(name), _initRootDomain()} {}
    Synchronization(std::string name, std::shared_ptr<SyncDomain> domain)
        : _name{std::move(name)}, _mutex{_initMutex()}, _domain{std::move(domain)} {}

    Lock acquireLock() {
        return Lock(_mutex);
    }

    void notifyOne() {
        _cv.notify_one();
    }

    void notifyAll() {
        _cv.notify_all();
    }

    template <typename P>
    void wait(Lock& lk, const P& p) {
        _cv.wait(lk, p);
    }

    template <typename Dur, typename P>
    bool waitFor(Lock& lk, const Dur& dur, const P& p) {
        return _cv.wait_for(lk, dur.toSystemDuration(), p);
    }

    const std::shared_ptr<SyncDomain>& domain() const {
        return _domain;
    }

private:
    std::string _name;
    MyMutex _mutex;
    stdx::condition_variable_any _cv;
    std::shared_ptr<SyncDomain> _domain;
};

}  // namespace mongo::transport

#undef MONGO_LOGV2_DEFAULT_COMPONENT
