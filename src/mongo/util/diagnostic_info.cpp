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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/util/diagnostic_info.h"

#include <forward_list>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/log.h"

using namespace fmt::literals;

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(currentOpSpawnsThreadWaitingForLatch);

constexpr auto kBlockedOpMutexName = "BlockedOpForTestLatch"_sd;
constexpr auto kBlockedOpInterruptibleName = "BlockedOpForTestInterruptible"_sd;

class BlockedOp {
public:
    void start(ServiceContext* serviceContext);
    void join();
    void setIsContended(bool value);
    void setIsWaiting(bool value);

private:
    stdx::condition_variable _cv;
    stdx::mutex _m;  // NOLINT

    struct LatchState {
        bool isContended = false;
        boost::optional<stdx::thread> thread{boost::none};

        Mutex mutex = MONGO_MAKE_LATCH(kBlockedOpMutexName);
    };
    LatchState _latchState;

    struct InterruptibleState {
        bool isWaiting = false;
        boost::optional<stdx::thread> thread{boost::none};

        stdx::condition_variable cv;
        Mutex mutex = MONGO_MAKE_LATCH(kBlockedOpInterruptibleName);
        bool isDone = false;
    };
    InterruptibleState _interruptibleState;
} gBlockedOp;

// This function causes us to make an additional thread with a self-contended lock so that
// $currentOp can observe its DiagnosticInfo. Note that we track each thread that called us so that
// we can join the thread when they are gone.
void BlockedOp::start(ServiceContext* serviceContext) {
    stdx::unique_lock<stdx::mutex> lk(_m);

    invariant(!_latchState.thread);
    invariant(!_interruptibleState.thread);

    _latchState.mutex.lock();
    _latchState.thread = stdx::thread([this, serviceContext]() mutable {
        ThreadClient tc("DiagnosticCaptureTestLatch", serviceContext);

        log() << "Entered currentOpSpawnsThreadWaitingForLatch thread";

        stdx::lock_guard testLock(_latchState.mutex);

        log() << "Joining currentOpSpawnsThreadWaitingForLatch thread";
    });

    _interruptibleState.thread = stdx::thread([this, serviceContext]() mutable {
        ThreadClient tc("DiagnosticCaptureTestInterruptible", serviceContext);
        auto opCtx = tc->makeOperationContext();

        log() << "Entered currentOpSpawnsThreadWaitingForLatch thread for interruptibles";
        stdx::unique_lock lk(_interruptibleState.mutex);
        opCtx->waitForConditionOrInterrupt(
            _interruptibleState.cv, lk, [&] { return _interruptibleState.isDone; });
        _interruptibleState.isDone = false;

        log() << "Joining currentOpSpawnsThreadWaitingForLatch thread for interruptibles";
    });


    _cv.wait(lk, [this] { return _latchState.isContended && _interruptibleState.isWaiting; });
    log() << "Started threads for currentOpSpawnsThreadWaitingForLatch";
}

// This function unlocks testMutex and joins if there are no more callers of BlockedOp::start()
// remaining
void BlockedOp::join() {
    decltype(_latchState.thread) latchThread;
    decltype(_interruptibleState.thread) interruptibleThread;
    {
        stdx::lock_guard<stdx::mutex> lk(_m);

        invariant(_latchState.thread);
        invariant(_interruptibleState.thread);

        _latchState.mutex.unlock();
        _latchState.isContended = false;

        {
            stdx::lock_guard lk(_interruptibleState.mutex);
            _interruptibleState.isDone = true;
            _interruptibleState.cv.notify_one();
        }
        _interruptibleState.isWaiting = false;

        std::swap(_latchState.thread, latchThread);
        std::swap(_interruptibleState.thread, interruptibleThread);
    }

    latchThread->join();
    interruptibleThread->join();
}

void BlockedOp::setIsContended(bool value) {
    log() << "Setting isContended to " << (value ? "true" : "false");
    stdx::lock_guard lk(_m);
    _latchState.isContended = value;
    _cv.notify_one();
}

void BlockedOp::setIsWaiting(bool value) {
    log() << "Setting isWaiting to " << (value ? "true" : "false");
    stdx::lock_guard lk(_m);
    _interruptibleState.isWaiting = value;
    _cv.notify_one();
}

struct DiagnosticInfoHandle {
    stdx::mutex mutex;  // NOLINT
    std::forward_list<DiagnosticInfo> list;
};
const auto getDiagnosticInfoHandle = Client::declareDecoration<DiagnosticInfoHandle>();

MONGO_INITIALIZER(LockListener)(InitializerContext* context) {
    class LockListener : public Mutex::LockListener {
        void onContendedLock(const StringData& name) override {
            if (auto client = Client::getCurrent()) {
                auto& handle = getDiagnosticInfoHandle(client);
                stdx::lock_guard<stdx::mutex> lk(handle.mutex);
                handle.list.emplace_front(DiagnosticInfo::capture(name));

                if (currentOpSpawnsThreadWaitingForLatch.shouldFail() &&
                    (name == kBlockedOpMutexName)) {
                    gBlockedOp.setIsContended(true);
                }
            }
        }

        void onQuickLock(const StringData&) override {
            // Do nothing
        }

        void onSlowLock(const StringData& name) override {
            if (auto client = Client::getCurrent()) {
                auto& handle = getDiagnosticInfoHandle(client);
                stdx::lock_guard<stdx::mutex> lk(handle.mutex);

                invariant(!handle.list.empty());
                handle.list.pop_front();
            }
        }

        void onUnlock(const StringData&) override {
            // Do nothing
        }
    };

    // Intentionally leaked, people use Latches in detached threads
    static auto& listener = *new LockListener;
    Mutex::addLockListener(&listener);

    return Status::OK();
}

MONGO_INITIALIZER(InterruptibleWaitListener)(InitializerContext* context) {
    class WaitListener : public Interruptible::WaitListener {
        using WakeReason = Interruptible::WakeReason;
        using WakeSpeed = Interruptible::WakeSpeed;

        void addInfo(const StringData& name) {
            if (auto client = Client::getCurrent()) {
                auto& handle = getDiagnosticInfoHandle(client);
                stdx::lock_guard<stdx::mutex> lk(handle.mutex);
                handle.list.emplace_front(DiagnosticInfo::capture(name));

                if (currentOpSpawnsThreadWaitingForLatch.shouldFail() &&
                    (name == kBlockedOpInterruptibleName)) {
                    gBlockedOp.setIsWaiting(true);
                }
            }
        }

        void removeInfo(const StringData& name) {
            if (auto client = Client::getCurrent()) {
                auto& handle = getDiagnosticInfoHandle(client);
                stdx::lock_guard<stdx::mutex> lk(handle.mutex);

                invariant(!handle.list.empty());
                handle.list.pop_front();
            }
        }

        void onLongSleep(const StringData& name) override {
            addInfo(name);
        }

        void onWake(const StringData& name, WakeReason, WakeSpeed speed) override {
            if (speed == WakeSpeed::kSlow) {
                removeInfo(name);
            }
        }
    };

    // Intentionally leaked, people can use in detached threads
    static auto& listener = *new WaitListener();
    Interruptible::addWaitListener(&listener);

    return Status::OK();
}

}  // namespace

bool operator==(const DiagnosticInfo& info1, const DiagnosticInfo& info2) {
    return info1._captureName == info2._captureName && info1._timestamp == info2._timestamp &&
        info1._backtrace.data == info2._backtrace.data;
}

std::string DiagnosticInfo::toString() const {
    return "{{ \"name\": \"{}\", \"time\": \"{}\", \"backtraceSize\": {} }}"_format(
        _captureName.toString(), _timestamp.toString(), _backtrace.data.size());
}

DiagnosticInfo DiagnosticInfo::capture(const StringData& captureName, Options options) {
    // Since we don't have a fast enough backtrace implementation at the moment, the Backtrace is
    // always empty. If SERVER-44091 happens, this should branch on options.shouldTakeBacktrace
    auto backtrace = Backtrace{};
    auto currentTime = getGlobalServiceContext()->getFastClockSource()->now();

    return DiagnosticInfo(currentTime, captureName, std::move(backtrace));
}

DiagnosticInfo::BlockedOpGuard::~BlockedOpGuard() {
    gBlockedOp.join();
}

auto DiagnosticInfo::maybeMakeBlockedOpForTest(Client* client) -> std::unique_ptr<BlockedOpGuard> {
    std::unique_ptr<BlockedOpGuard> guard;
    currentOpSpawnsThreadWaitingForLatch.executeIf(
        [&](const BSONObj&) {
            gBlockedOp.start(client->getServiceContext());
            guard = std::make_unique<BlockedOpGuard>();
        },
        [&](const BSONObj& data) {
            return data.hasField("clientName") &&
                (data.getStringField("clientName") == client->desc());
        });

    return guard;
}

boost::optional<DiagnosticInfo> DiagnosticInfo::get(Client& client) {
    auto& handle = getDiagnosticInfoHandle(client);
    stdx::lock_guard<stdx::mutex> lk(handle.mutex);

    if (handle.list.empty()) {
        return boost::none;
    }

    return handle.list.front();
}

}  // namespace mongo
