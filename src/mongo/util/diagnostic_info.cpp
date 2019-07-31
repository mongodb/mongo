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

#if defined(__linux__)
#include <elf.h>
#include <execinfo.h>
#include <link.h>
#endif

#include <fmt/format.h>
#include <fmt/ostream.h>

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"

using namespace fmt::literals;

namespace mongo {
// Maximum number of stack frames to appear in a backtrace.
const unsigned int kMaxBackTraceFrames = 100;
MONGO_FAIL_POINT_DEFINE(keepDiagnosticCaptureOnFailedLock);

namespace {
const auto gDiagnosticHandle = Client::declareDecoration<DiagnosticInfo::Diagnostic>();

MONGO_INITIALIZER(LockActions)(InitializerContext* context) {

    class LockActionsSubclass : public LockActions {
        void onContendedLock(const StringData& name) override {
            if (haveClient()) {
                DiagnosticInfo::Diagnostic::set(
                    Client::getCurrent(),
                    std::make_shared<DiagnosticInfo>(takeDiagnosticInfo(name)));
            }
        }
        void onUnlock() override {
            DiagnosticInfo::Diagnostic::clearDiagnostic();
        }
        void onFailedLock() override {
            if (!MONGO_FAIL_POINT(keepDiagnosticCaptureOnFailedLock)) {
                DiagnosticInfo::Diagnostic::clearDiagnostic();
            }
        }
    };
    std::unique_ptr<LockActions> myPointer = std::make_unique<LockActionsSubclass>();
    Mutex::setLockActions(std::move(myPointer));

    return Status::OK();
}
}  // namespace

auto DiagnosticInfo::Diagnostic::get(Client* const client) -> std::shared_ptr<DiagnosticInfo> {
    auto& handle = gDiagnosticHandle(client);
    stdx::lock_guard lk(handle.m);
    return handle.diagnostic;
}

void DiagnosticInfo::Diagnostic::set(Client* const client,
                                     std::shared_ptr<DiagnosticInfo> newDiagnostic) {
    auto& handle = gDiagnosticHandle(client);
    stdx::lock_guard lk(handle.m);
    handle.diagnostic = newDiagnostic;
}

void DiagnosticInfo::Diagnostic::clearDiagnostic() {
    if (haveClient()) {
        DiagnosticInfo::Diagnostic::set(Client::getCurrent(), nullptr);
    }
}

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

}  // anonymous namespace

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

// iterates through the backtrace instruction pointers to
// find the instruction pointer that refers to a segment in the addr_map
DiagnosticInfo::StackTrace DiagnosticInfo::makeStackTrace() const {
    DiagnosticInfo::StackTrace trace;
    for (auto addr : _backtraceAddresses) {
        trace.frames.emplace_back(gDynamicObjectMap.getFrame(addr));
    }
    return trace;
}

static std::vector<void*> getBacktraceAddresses() {
    std::vector<void*> backtraceAddresses(kMaxBackTraceFrames, 0);
    int addressCount = backtrace(backtraceAddresses.data(), kMaxBackTraceFrames);
    // backtrace will modify the vector's underlying array without updating its size
    backtraceAddresses.resize(static_cast<unsigned int>(addressCount));
    return backtraceAddresses;
}
#else
DiagnosticInfo::StackTrace DiagnosticInfo::makeStackTrace() const {
    return DiagnosticInfo::StackTrace();
}

static std::vector<void*> getBacktraceAddresses() {
    return std::vector<void*>();
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
        info1._backtraceAddresses == info2._backtraceAddresses;
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
        _captureName.toString(), _timestamp.toString(), _backtraceAddresses.size());
}

DiagnosticInfo takeDiagnosticInfo(const StringData& captureName) {
    // uses backtrace to retrieve an array of instruction pointers for currently active
    // function calls of the program
    return DiagnosticInfo(getGlobalServiceContext()->getFastClockSource()->now(),
                          captureName,
                          getBacktraceAddresses());
}
}  // namespace mongo
