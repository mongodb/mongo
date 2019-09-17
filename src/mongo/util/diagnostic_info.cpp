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

#include "mongo/config.h"

#if defined(__linux__)
#include <elf.h>
#include <link.h>
#endif

#if defined(MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE)
#include <execinfo.h>
#endif

#include <fmt/format.h>
#include <fmt/ostream.h>

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/platform/condition_variable.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"

using namespace fmt::literals;

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(currentOpSpawnsThreadWaitingForLatch);

constexpr auto kBlockedOpMutexName = "BlockedOpForTest"_sd;

class BlockedOp {
public:
    void start(ServiceContext* serviceContext);
    void join();
    void setIsContended(bool value);

private:
    Mutex _testMutex = MONGO_MAKE_LATCH(kBlockedOpMutexName);

    stdx::condition_variable _cv;
    stdx::mutex _m;  // NOLINT

    struct State {
        bool isContended = false;
        boost::optional<stdx::thread> thread{boost::none};
    };
    State _state;
} gBlockedOp;

// This function causes us to make an additional thread with a self-contended lock so that
// $currentOp can observe its DiagnosticInfo. Note that we track each thread that called us so that
// we can join the thread when they are gone.
void BlockedOp::start(ServiceContext* serviceContext) {
    stdx::unique_lock<stdx::mutex> lk(_m);

    invariant(!_state.thread);

    _testMutex.lock();
    _state.thread = stdx::thread([this, serviceContext]() mutable {
        ThreadClient tc("DiagnosticCaptureTest", serviceContext);

        log() << "Entered currentOpSpawnsThreadWaitingForLatch thread";

        stdx::lock_guard testLock(_testMutex);

        log() << "Joining currentOpSpawnsThreadWaitingForLatch thread";
    });

    _cv.wait(lk, [this] { return _state.isContended; });
    log() << "Started thread for currentOpSpawnsThreadWaitingForLatch";
}

// This function unlocks testMutex and joins if there are no more callers of BlockedOp::start()
// remaining
void BlockedOp::join() {
    auto thread = [&] {
        stdx::lock_guard<stdx::mutex> lk(_m);

        invariant(_state.thread);

        _testMutex.unlock();

        _state.isContended = false;
        _cv.notify_one();

        return std::exchange(_state.thread, boost::none);
    }();
    thread->join();
}

void BlockedOp::setIsContended(bool value) {
    log() << "Setting isContended to " << (value ? "true" : "false");
    stdx::lock_guard lk(_m);
    _state.isContended = value;
    _cv.notify_one();
}

struct DiagnosticInfoHandle {
    stdx::mutex mutex;  // NOLINT
    boost::optional<DiagnosticInfo> maybeInfo = boost::none;
};
const auto getDiagnosticInfoHandle = Client::declareDecoration<DiagnosticInfoHandle>();

MONGO_INITIALIZER(LockActions)(InitializerContext* context) {
    class LockActionsSubclass : public Mutex::LockActions {
        void onContendedLock(const StringData& name) override {
            auto client = Client::getCurrent();
            if (client) {
                auto& handle = getDiagnosticInfoHandle(client);
                stdx::lock_guard<stdx::mutex> lk(handle.mutex);
                handle.maybeInfo.emplace(DiagnosticInfo::capture(name));

                if (currentOpSpawnsThreadWaitingForLatch.shouldFail() &&
                    (name == kBlockedOpMutexName)) {
                    gBlockedOp.setIsContended(true);
                }
            }
        }
        void onUnlock(const StringData& name) override {
            auto client = Client::getCurrent();
            if (client) {
                auto& handle = getDiagnosticInfoHandle(client);
                stdx::lock_guard<stdx::mutex> lk(handle.mutex);
                handle.maybeInfo.reset();
            }
        }
    };

    // Intentionally leaked, people use Latches in detached threads
    static auto& actions = *new LockActionsSubclass;
    Mutex::LockActions::add(&actions);

    return Status::OK();
}

/*
MONGO_INITIALIZER(ConditionVariableActions)(InitializerContext* context) {

    class ConditionVariableActionsSubclass : public ConditionVariableActions {
        void onUnfulfilledConditionVariable(const StringData& name) override {
            if (haveClient()) {
                DiagnosticInfo::Diagnostic::set(
                    Client::getCurrent(),
                    std::make_shared<DiagnosticInfo>(capture(name)));
            }
        }
        void onFulfilledConditionVariable() override {
            DiagnosticInfo::Diagnostic::clearDiagnostic();
        }
    };

    std::unique_ptr<ConditionVariableActions> conditionVariablePointer =
        std::make_unique<ConditionVariableActionsSubclass>();
    ConditionVariable::setConditionVariableActions(std::move(conditionVariablePointer));

    return Status::OK();
}
*/

}  // namespace

#if defined(__linux__)
namespace {

class DynamicObjectMap {
public:
    struct Section {
        StringData objectPath;
        uintptr_t sectionOffset = 0;
        size_t sectionSize = 0;
    };

    // uses dl_iterate_phdr to retrieve information on the shared objects that have been loaded in
    // the form of a map from segment address to object
    void init() {
        ::dl_iterate_phdr(addToMap, &_map);
    }

    // return a StackFrame object located at the header address in the map that is immediately
    // preceding or equal to the instruction pointer address
    DiagnosticInfo::StackFrame getFrame(void* instructionPtr) const;

    // callback function in dl_iterate_phdr that iterates through the shared objects and creates a
    // map from the section address to object
    static int addToMap(dl_phdr_info* info, size_t size, void* data);

private:
    std::map<uintptr_t, Section> _map;
} gDynamicObjectMap;

MONGO_INITIALIZER(InitializeDynamicObjectMap)(InitializerContext* context) {
    gDynamicObjectMap.init();
    return Status::OK();
};

int DynamicObjectMap::addToMap(dl_phdr_info* info, size_t size, void* data) {
    auto& addr_map = *reinterpret_cast<decltype(DynamicObjectMap::_map)*>(data);
    for (int j = 0; j < info->dlpi_phnum; j++) {
        auto& header = info->dlpi_phdr[j];
        auto addr = info->dlpi_addr + header.p_vaddr;
        switch (header.p_type) {
            case PT_LOAD:
            case PT_DYNAMIC:
                break;
            default:
                continue;
        }
        auto frame = Section{
            info->dlpi_name,                          // object name
            static_cast<uintptr_t>(header.p_offset),  // section offset in file
            header.p_memsz,                           // section size in memory
        };
        addr_map.emplace(addr, frame);
    }
    return 0;
}

DiagnosticInfo::StackFrame DynamicObjectMap::getFrame(void* instructionPtr) const {
    auto address = reinterpret_cast<uintptr_t>(instructionPtr);

    // instructionPtr < it->first
    auto it = _map.upper_bound(address);
    // instuctionPtr >= it->first
    --it;

    auto& [headerAddress, frame] = *it;
    dassert(address < (headerAddress + frame.sectionSize));

    auto fileOffset = address - headerAddress + frame.sectionOffset;
    return DiagnosticInfo::StackFrame{frame.objectPath, fileOffset};
}

}  // namespace
#endif  // linux

#if defined(MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE) && defined(__linux__)
// iterates through the backtrace instruction pointers to
// find the instruction pointer that refers to a segment in the addr_map
DiagnosticInfo::StackTrace DiagnosticInfo::makeStackTrace() const {
    DiagnosticInfo::StackTrace trace;
    for (auto address : _backtrace.data) {
        trace.frames.emplace_back(gDynamicObjectMap.getFrame(address));
    }
    return trace;
}

auto DiagnosticInfo::getBacktrace() -> Backtrace {
    Backtrace list;
    auto len = ::backtrace(list.data.data(), list.data.size());
    list.data.resize(len);
    return list;
}
#else
DiagnosticInfo::StackTrace DiagnosticInfo::makeStackTrace() const {
    return DiagnosticInfo::StackTrace();
}

auto DiagnosticInfo::getBacktrace() -> Backtrace {
    return {};
}
#endif

bool operator==(const DiagnosticInfo::StackFrame& frame1,
                const DiagnosticInfo::StackFrame& frame2) {
    return frame1.objectPath == frame2.objectPath &&
        frame1.instructionOffset == frame2.instructionOffset;
}

bool operator==(const DiagnosticInfo::StackTrace& trace1,
                const DiagnosticInfo::StackTrace& trace2) {
    return trace1.frames == trace2.frames;
}

bool operator==(const DiagnosticInfo& info1, const DiagnosticInfo& info2) {
    return info1._captureName == info2._captureName && info1._timestamp == info2._timestamp &&
        info1._backtrace.data == info2._backtrace.data;
}

std::string DiagnosticInfo::StackFrame::toString() const {
    return "{{ \"path\": \"{}\", \"addr\": \"0x{:x}\" }}"_format(objectPath, instructionOffset);
}

std::string DiagnosticInfo::StackTrace::toString() const {
    str::stream stream;
    stream << "{ \"backtrace\": [ ";
    bool isFirst = true;
    for (auto& frame : frames) {
        if (!std::exchange(isFirst, false)) {
            // Sadly, JSON doesn't allow trailing commas
            stream << ", ";
        }
        stream << frame.toString();
    }
    stream << "] }";
    return stream;
}

std::string DiagnosticInfo::toString() const {
    return "{{ \"name\": \"{}\", \"time\": \"{}\", \"backtraceSize\": {} }}"_format(
        _captureName.toString(), _timestamp.toString(), _backtrace.data.size());
}

DiagnosticInfo DiagnosticInfo::capture(const StringData& captureName, Options options) {
    // uses backtrace to retrieve an array of instruction pointers for currently active
    // function calls of the program
    return DiagnosticInfo(getGlobalServiceContext()->getFastClockSource()->now(),
                          captureName,
                          options.shouldTakeBacktrace ? DiagnosticInfo::getBacktrace()
                                                      : Backtrace{{}});
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
    return handle.maybeInfo;
}

}  // namespace mongo
