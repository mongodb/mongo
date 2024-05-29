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


namespace mongo {

namespace {
using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(currentOpSpawnsThreadWaitingForLatch);

constexpr auto kBlockedOpMutexName = "BlockedOpForTestLatch"_sd;
constexpr auto kBlockedOpInterruptibleName = "BlockedOpForTestInterruptible"_sd;

struct LatchState {
    void start(Service* service) {
        mutex.lock();
        thread = stdx::thread([this, service]() {
            ThreadClient tc("DiagnosticCaptureTestLatch", service);

            // TODO(SERVER-74659): Please revisit if this thread could be made killable.
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc.get()->setSystemOperationUnkillableByStepdown(lk);
            }

            LOGV2(23123, "Entered currentOpSpawnsThreadWaitingForLatch thread");

            stdx::lock_guard testLock(mutex);

            LOGV2(23124, "Joining currentOpSpawnsThreadWaitingForLatch thread");
        });
    }

    void stop() {
        mutex.unlock();
    }

    boost::optional<Promise<void>> readyReached;
    stdx::thread thread;

    Mutex mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(3), kBlockedOpMutexName);
};

struct InterruptibleState {
    void start(Service* service) {
        thread = stdx::thread([this, service]() mutable {
            ThreadClient tc("DiagnosticCaptureTestInterruptible", service);

            // TODO(SERVER-74659): Please revisit if this thread could be made killable.
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc.get()->setSystemOperationUnkillableByStepdown(lk);
            }

            auto opCtx = tc->makeOperationContext();

            LOGV2(23125, "Entered currentOpSpawnsThreadWaitingForLatch thread for interruptibles");
            stdx::unique_lock lk(mutex);
            opCtx->waitForConditionOrInterrupt(cv, lk, [&] { return isDone; });
            isDone = false;

            LOGV2(23126, "Joining currentOpSpawnsThreadWaitingForLatch thread for interruptibles");
        });
    }

    void stop() {
        stdx::lock_guard lk(mutex);
        isDone = true;
        cv.notify_one();
    }

    boost::optional<Promise<void>> readyReached;
    stdx::thread thread;

    stdx::condition_variable cv;
    Mutex mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), kBlockedOpInterruptibleName);
    bool isDone = false;
};

class BlockedOp {
public:
    void start(Service* service);
    void join();
    void setIsContended();
    void setIsWaiting();

private:
    stdx::mutex _m;  // NOLINT
    LatchState _latchState;
    InterruptibleState _interruptibleState;
};

BlockedOp gBlockedOp;

// Starts a blocked operation. Makes two threads - one that contends for a lock held throughout the
// blocked operation, and one that waits for a condition throughout the blocked operation.
void BlockedOp::start(Service* service) {
    stdx::lock_guard lk(_m);

    invariant(!_latchState.readyReached);
    invariant(!_interruptibleState.readyReached);

    auto latchStateReady = makePromiseFuture<void>();
    auto interruptibleStateReady = makePromiseFuture<void>();
    _latchState.readyReached = std::move(latchStateReady.promise);
    _interruptibleState.readyReached = std::move(interruptibleStateReady.promise);

    _latchState.start(service);
    _interruptibleState.start(service);

    latchStateReady.future.get();
    interruptibleStateReady.future.get();

    LOGV2(23127, "Started threads for currentOpSpawnsThreadWaitingForLatch");
}

// Unblocks threads started in BlockedOp::start(), joins them, and returns the BlockedOp to its
// initial state.
void BlockedOp::join() {
    stdx::unique_lock lk(_m);
    _latchState.stop();
    _interruptibleState.stop();
    auto latchThread = std::exchange(_latchState.thread, {});
    auto interruptibleThread = std::exchange(_interruptibleState.thread, {});
    lk.unlock();

    latchThread.join();
    interruptibleThread.join();
}

void BlockedOp::setIsContended() {
    LOGV2(23128, "Setting isContended");
    if (auto promise = std::exchange(_latchState.readyReached, {}))
        promise->emplaceValue();
}

void BlockedOp::setIsWaiting() {
    LOGV2(23129, "Setting isWaiting");
    if (auto promise = std::exchange(_interruptibleState.readyReached, {}))
        promise->emplaceValue();
}

struct DiagnosticInfoHandle {
    stdx::mutex mutex;  // NOLINT
    std::forward_list<DiagnosticInfo> list;
};
const auto getDiagnosticInfoHandle = Client::declareDecoration<DiagnosticInfoHandle>();

MONGO_INITIALIZER_GENERAL(DiagnosticInfo, (), ("FinalizeDiagnosticListeners"))
(InitializerContext*) {
    class DiagnosticListener : public latch_detail::DiagnosticListener {
        void onContendedLock(const Identity& id) override {
            if (auto client = Client::getCurrent()) {
                DiagnosticInfo::capture(client, id.name());

                if (currentOpSpawnsThreadWaitingForLatch.shouldFail() &&
                    (id.name() == kBlockedOpMutexName)) {
                    gBlockedOp.setIsContended();
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
                    gBlockedOp.setIsWaiting();
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
