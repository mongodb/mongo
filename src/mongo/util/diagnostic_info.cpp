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

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/platform/basic.h"
#include "mongo/platform/mutex.h"

#include "mongo/util/diagnostic_info.h"

#if defined(__linux__)
#include <elf.h>
#include <execinfo.h>
#include <link.h>
#endif

#include "mongo/util/clock_source.h"

namespace mongo {
// Maximum number of stack frames to appear in a backtrace.
const unsigned int kMaxBackTraceFrames = 100;

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
            if (haveClient()) {
                DiagnosticInfo::Diagnostic::set(Client::getCurrent(), nullptr);
            }
        }
    };
    std::unique_ptr<LockActions> myPointer = std::make_unique<LockActionsSubclass>();
    Mutex::setLockActions(std::move(myPointer));

    return Status::OK();
}
}  // namespace

auto DiagnosticInfo::Diagnostic::get(Client* const client) -> DiagnosticInfo& {
    auto& handle = gDiagnosticHandle(client);
    stdx::lock_guard lk(handle.m);
    return *handle.diagnostic;
}

void DiagnosticInfo::Diagnostic::set(Client* const client,
                                     std::shared_ptr<DiagnosticInfo> newDiagnostic) {
    auto& handle = gDiagnosticHandle(client);
    stdx::lock_guard lk(handle.m);
    handle.diagnostic = newDiagnostic;
}

#if defined(__linux__)
struct DynamicObject {
    StringData objectPath;
    ptrdiff_t sectionOffset;
    unsigned long sectionSize;
};

// callback function in dl_iterate_phdr that iterates through the shared objects
// and creates a map from the section address to object
static int createStackTraceMap(dl_phdr_info* info, size_t size, void* data) {
    auto& addr_map = *reinterpret_cast<std::map<void*, DynamicObject>*>(data);
    for (int j = 0; j < info->dlpi_phnum; j++) {
        auto addr = (void*)(info->dlpi_addr + info->dlpi_phdr[j].p_vaddr);
        DynamicObject frame = {
            info->dlpi_name,                        // name
            (ptrdiff_t)info->dlpi_phdr[j].p_vaddr,  // section offset
            info->dlpi_phdr[j].p_memsz,             // section size
        };
        addr_map.emplace(addr, frame);
    }
    return 0;
}

class DynamicObjectMap {
public:
    DynamicObjectMap() {
        // uses dl_iterate_phdr to retrieve information on the shared objects that have been loaded
        // in the form of a map from segment address to object
        dl_iterate_phdr(createStackTraceMap, (void*)&_map);
    }

    // return a StackFrameObject located at the header address in the map
    // that is immediately preceding or equal to the instruction pointer
    // address
    DiagnosticInfo::StackFrame getFrame(void* instructionPtr) const {
        auto it = --_map.upper_bound(instructionPtr);

        auto & [ objectPtr, frame ] = *it;
        ptrdiff_t instructionOffset =
            static_cast<char*>(instructionPtr) - static_cast<char*>(objectPtr);
        return DiagnosticInfo::StackFrame{
            frame.objectPath, frame.sectionOffset, frame.sectionSize, instructionOffset};
    }

    friend int createStackTraceMap(dl_phdr_info* info, size_t size, void* data);

private:
    std::map<void*, DynamicObject> _map;
} gDynamicObjectMap;

// iterates through the backtrace instruction pointers to
// find the instruction pointer that refers to a segment in the addr_map
DiagnosticInfo::StackTrace DiagnosticInfo::makeStackTrace() {
    DiagnosticInfo::StackTrace stacktrace;
    for (auto addr : _backtraceAddresses) {
        stacktrace.emplace_back(gDynamicObjectMap.getFrame(addr));
    }
    return stacktrace;
}

static std::vector<void*> getBacktraceAddresses() {
    std::vector<void*> backtraceAddresses(kMaxBackTraceFrames, 0);
    int addressCount = backtrace(backtraceAddresses.data(), kMaxBackTraceFrames);
    // backtrace will modify the vector's underlying array without updating its size
    backtraceAddresses.resize(static_cast<unsigned int>(addressCount));
    return backtraceAddresses;
}
#else
DiagnosticInfo::StackTrace DiagnosticInfo::makeStackTrace() {
    return DiagnosticInfo::StackTrace();
}

static std::vector<void*> getBacktraceAddresses() {
    return std::vector<void*>();
}
#endif

DiagnosticInfo takeDiagnosticInfo(const StringData& captureName) {
    // uses backtrace to retrieve an array of instruction pointers for currently active
    // function calls of the program
    return DiagnosticInfo(getGlobalServiceContext()->getFastClockSource()->now(),
                          captureName,
                          getBacktraceAddresses());
}
}  // namespace mongo
