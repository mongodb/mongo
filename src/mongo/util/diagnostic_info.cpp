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


#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include <forward_list>
#include <mutex>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/diagnostic_info.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/interruptible.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


using namespace fmt::literals;

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(currentOpSpawnsThreadWaitingForLatch);

constexpr auto kBlockedOpMutexName = "BlockedOpForTestLatch"_sd;
constexpr auto kBlockedOpInterruptibleName = "BlockedOpForTestInterruptible"_sd;

class BlockedOp {
public:
    void start(Service* service);
    void join();
    void setIsContended(bool value);
    void setIsWaiting(bool value);

private:
    stdx::condition_variable _cv;
    stdx::mutex _m;  // NOLINT

    struct LatchState {
        bool isContended = false;
        boost::optional<stdx::thread> thread{boost::none};

        Mutex mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(3), kBlockedOpMutexName);
    };
    LatchState _latchState;

    struct InterruptibleState {
        bool isWaiting = false;
        boost::optional<stdx::thread> thread{boost::none};

        stdx::condition_variable cv;
        Mutex mutex =
            MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), kBlockedOpInterruptibleName);
        bool isDone = false;
    };
    InterruptibleState _interruptibleState;
} gBlockedOp;

// This function causes us to make an additional thread with a self-contended lock so that
// $currentOp can observe its DiagnosticInfo. Note that we track each thread that called us so that
// we can join the thread when they are gone.
void BlockedOp::start(Service* service) {
    stdx::unique_lock<stdx::mutex> lk(_m);

    invariant(!_latchState.thread);
    invariant(!_interruptibleState.thread);

    _latchState.mutex.lock();
    _latchState.thread = stdx::thread([this, service]() mutable {
        ThreadClient tc("DiagnosticCaptureTestLatch", service);

        // TODO(SERVER-74659): Please revisit if this thread could be made killable.
        {
            stdx::lock_guard<Client> lk(*tc.get());
            tc.get()->setSystemOperationUnkillableByStepdown(lk);
        }

        LOGV2(23123, "Entered currentOpSpawnsThreadWaitingForLatch thread");

        stdx::lock_guard testLock(_latchState.mutex);

        LOGV2(23124, "Joining currentOpSpawnsThreadWaitingForLatch thread");
    });

    _interruptibleState.thread = stdx::thread([this, service]() mutable {
        ThreadClient tc("DiagnosticCaptureTestInterruptible", service);

        // TODO(SERVER-74659): Please revisit if this thread could be made killable.
        {
            stdx::lock_guard<Client> lk(*tc.get());
            tc.get()->setSystemOperationUnkillableByStepdown(lk);
        }

        auto opCtx = tc->makeOperationContext();

        LOGV2(23125, "Entered currentOpSpawnsThreadWaitingForLatch thread for interruptibles");
        stdx::unique_lock lk(_interruptibleState.mutex);
        opCtx->waitForConditionOrInterrupt(
            _interruptibleState.cv, lk, [&] { return _interruptibleState.isDone; });
        _interruptibleState.isDone = false;

        LOGV2(23126, "Joining currentOpSpawnsThreadWaitingForLatch thread for interruptibles");
    });


    _cv.wait(lk, [this] { return _latchState.isContended && _interruptibleState.isWaiting; });
    LOGV2(23127, "Started threads for currentOpSpawnsThreadWaitingForLatch");
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
    LOGV2(23128, "Setting isContended", "value"_attr = (value ? "true" : "false"));
    stdx::lock_guard lk(_m);
    _latchState.isContended = value;
    _cv.notify_one();
}

void BlockedOp::setIsWaiting(bool value) {
    LOGV2(23129, "Setting isWaiting", "value"_attr = (value ? "true" : "false"));
    stdx::lock_guard lk(_m);
    _interruptibleState.isWaiting = value;
    _cv.notify_one();
}

struct DiagnosticInfoHandle {
    stdx::mutex mutex;  // NOLINT
    std::forward_list<DiagnosticInfo> list;
};
const auto getDiagnosticInfoHandle = Client::declareDecoration<DiagnosticInfoHandle>();

MONGO_INITIALIZER_GENERAL(DiagnosticInfo, (/* NO PREREQS */), ("FinalizeDiagnosticListeners"))
(InitializerContext* context) {
    class DiagnosticListener : public latch_detail::DiagnosticListener {
        void onContendedLock(const Identity& id) override {
            if (auto client = Client::getCurrent()) {
                DiagnosticInfo::capture(client, id.name());

                if (currentOpSpawnsThreadWaitingForLatch.shouldFail() &&
                    (id.name() == kBlockedOpMutexName)) {
                    gBlockedOp.setIsContended(true);
                }
            }
        }

        void onQuickLock(const Identity&) override {
            // Do nothing
        }

        void onSlowLock(const Identity& id) override {
            if (auto client = Client::getCurrent()) {
                auto& handle = getDiagnosticInfoHandle(client);
                stdx::lock_guard<stdx::mutex> lk(handle.mutex);

                invariant(!handle.list.empty());
                handle.list.pop_front();
            }
        }

        void onUnlock(const Identity&) override {
            // Do nothing
        }
    };

    latch_detail::installDiagnosticListener<DiagnosticListener>();
}

MONGO_INITIALIZER(InterruptibleWaitListener)(InitializerContext* context) {
    class WaitListener : public Interruptible::WaitListener {
        using WakeReason = Interruptible::WakeReason;
        using WakeSpeed = Interruptible::WakeSpeed;

        void addInfo(StringData name) {
            if (auto client = Client::getCurrent()) {
                DiagnosticInfo::capture(client, name);

                if (currentOpSpawnsThreadWaitingForLatch.shouldFail() &&
                    (name == kBlockedOpInterruptibleName)) {
                    gBlockedOp.setIsWaiting(true);
                }
            }
        }

        void removeInfo(StringData name) {
            if (auto client = Client::getCurrent()) {
                auto& handle = getDiagnosticInfoHandle(client);
                stdx::lock_guard<stdx::mutex> lk(handle.mutex);

                invariant(!handle.list.empty());
                handle.list.pop_front();
            }
        }

        void onLongSleep(StringData name) override {
            addInfo(name);
        }

        void onWake(StringData name, WakeReason, WakeSpeed speed) override {
            if (speed == WakeSpeed::kSlow) {
                removeInfo(name);
            }
        }
    };

    Interruptible::installWaitListener<WaitListener>();
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

const DiagnosticInfo& DiagnosticInfo::capture(Client* client,
                                              StringData captureName,
                                              Options options) noexcept {
    auto currentTime = client->getServiceContext()->getFastClockSource()->now();

    // Since we don't have a fast enough backtrace implementation at the moment, the Backtrace is
    // always empty. If SERVER-44091 happens, this should branch on options.shouldTakeBacktrace
    auto backtrace = Backtrace{};

    auto info = DiagnosticInfo(currentTime, captureName, std::move(backtrace));

    auto& handle = getDiagnosticInfoHandle(client);

    stdx::lock_guard<stdx::mutex> lk(handle.mutex);
    handle.list.emplace_front(std::move(info));

    return handle.list.front();
}

DiagnosticInfo::BlockedOpGuard::~BlockedOpGuard() {
    gBlockedOp.join();
}

auto DiagnosticInfo::maybeMakeBlockedOpForTest(Client* client) -> std::unique_ptr<BlockedOpGuard> {
    std::unique_ptr<BlockedOpGuard> guard;
    currentOpSpawnsThreadWaitingForLatch.executeIf(
        [&](const BSONObj&) {
            gBlockedOp.start(client->getService());
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
