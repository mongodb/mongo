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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/stacktrace.h"

#pragma warning(push)
// C4091: 'typedef ': ignored on left of '' when no variable is declared
#pragma warning(disable : 4091)
#include <DbgHelp.h>
#pragma warning(pop)

#include <boost/filesystem/operations.hpp>
#include <boost/optional.hpp>
#include <cstdio>
#include <cstdlib>
#include <fmt/format.h>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/text.h"

namespace mongo {
namespace {

const size_t kPathBufferSize = 1024;

struct Options {
    bool withHumanReadable = false;
    bool rawAddress = false;
};

// On Windows the symbol handler must be initialized at process startup and cleaned up at shutdown.
// This class wraps up that logic and gives access to the process handle associated with the
// symbol handler. Because access to the symbol handler API is not thread-safe, it also provides
// a lock/unlock method so the whole symbol handler can be used with a stdx::lock_guard.
class SymbolHandler {
    SymbolHandler(const SymbolHandler&) = delete;
    SymbolHandler& operator=(const SymbolHandler&) = delete;

public:
    SymbolHandler() {
        auto handle = GetCurrentProcess();

        std::wstring modulePath(kPathBufferSize, 0);
        const auto pathSize = GetModuleFileNameW(nullptr, &modulePath.front(), modulePath.size());
        invariant(pathSize != 0);
        modulePath.resize(pathSize);
        boost::filesystem::wpath exePath(modulePath);

        std::wstringstream symbolPathBuilder;
        symbolPathBuilder << exePath.parent_path().wstring()
                          << L";C:\\Windows\\System32;C:\\Windows";
        const auto symbolPath = symbolPathBuilder.str();

        if (!SymInitializeW(handle, symbolPath.c_str(), TRUE)) {
            LOGV2_ERROR(
                31443, "Stack trace initialization failed", "error"_attr = errnoWithDescription());
            return;
        }

        _processHandle = handle;
        _origOptions = SymGetOptions();
        SymSetOptions(_origOptions | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
    }

    ~SymbolHandler() {
        SymSetOptions(_origOptions);
        SymCleanup(getHandle());
    }

    HANDLE getHandle() const {
        return _processHandle.value();
    }

    explicit operator bool() const {
        return static_cast<bool>(_processHandle);
    }

    void lock() {
        _mutex.lock();
    }

    void unlock() {
        _mutex.unlock();
    }

    static SymbolHandler& instance() {
        static SymbolHandler globalSymbolHandler;
        return globalSymbolHandler;
    }

private:
    boost::optional<HANDLE> _processHandle;
    stdx::mutex _mutex;  // NOLINT
    DWORD _origOptions;
};

MONGO_INITIALIZER(IntializeSymbolHandler)(::mongo::InitializerContext* ctx) {
    // We call this to ensure that the symbol handler is initialized in a single-threaded
    // context. The constructor of SymbolHandler does all the error handling, so we don't need to
    // do anything with the return value. Just make sure it gets called.
    SymbolHandler::instance();

    // Initializing the symbol handler is not a fatal error, so we always return Status::OK() here.
    return Status::OK();
}

/**
 * Get the display name of the executable module containing the specified address.
 *
 * @param process               Process handle
 * @param address               Address to find
 */
static std::string getModuleName(HANDLE process, DWORD64 address) {
    IMAGEHLP_MODULE64 module64;
    memset(&module64, 0, sizeof(module64));
    module64.SizeOfStruct = sizeof(module64);
    if (!SymGetModuleInfo64(process, address, &module64))
        return {};
    char* moduleName = module64.LoadedImageName;
    if (char* backslash = strrchr(moduleName, '\\'); backslash)
        moduleName = backslash + 1;
    return moduleName;
}

/**
 * Get the display name and line number of the source file containing the specified address.
 *
 * @param process               Process handle
 * @param address               Address to find
 */
static std::pair<std::string, size_t> getSourceLocation(HANDLE process, DWORD64 address) {
    IMAGEHLP_LINE64 line64;
    memset(&line64, 0, sizeof(line64));
    line64.SizeOfStruct = sizeof(line64);
    DWORD offset;
    if (!SymGetLineFromAddr64(process, address, &offset, &line64))
        return {};
    std::string filename = line64.FileName;
    static constexpr const char* kDiscards[] = {R"(\src\mongo\)", R"(\src\third_party\)"};
    for (const char* const discard : kDiscards) {
        if (auto start = filename.find(discard); start != std::string::npos) {
            filename.replace(0, start, "...");
            break;
        }
    }
    for (char& c : filename)
        if (c == '\\')
            c = '/';
    return {std::move(filename), line64.LineNumber};
}

/**
 * Get the display text of the symbol and offset of the specified address.
 *
 * @param process                   Process handle
 * @param address                   Address to find
 * @param symbolInfo                Caller's pre-built SYMBOL_INFO struct (for efficiency)
 */
static std::pair<std::string, size_t> getSymbolAndOffset(HANDLE process,
                                                         DWORD64 address,
                                                         SYMBOL_INFO* symbolInfo) {
    DWORD64 offset;
    if (!SymFromAddr(process, address, &offset, symbolInfo))
        return {};
    return {symbolInfo->Name, offset};
}

struct TraceItem {
    uintptr_t address;
    std::string module;
    std::pair<std::string, size_t> source;
    std::pair<std::string, size_t> symbol;
};

void appendTrace(BSONObjBuilder* bob,
                 const std::vector<TraceItem>& traceList,
                 const Options& options) {
    auto bt = BSONArrayBuilder(bob->subarrayStart("backtrace"));
    for (const auto& item : traceList) {
        auto o = BSONObjBuilder(bt.subobjStart());
        if (options.rawAddress)
            o.append("a", stack_trace_detail::Hex(item.address));
        if (!item.module.empty())
            o.append("module", item.module);
        if (!item.source.first.empty()) {
            o.append("file", item.source.first);
            o.append("line", static_cast<int>(item.source.second));
        }
        if (!item.symbol.first.empty()) {
            o.append("s", item.symbol.first);
            o.append("s+", stack_trace_detail::Hex(item.symbol.second));
        }
    }
}

std::vector<TraceItem> makeTraceList(CONTEXT& context) {
    std::vector<TraceItem> traceList;
    auto& symbolHandler = SymbolHandler::instance();
    stdx::lock_guard<SymbolHandler> lk(symbolHandler);

    if (!symbolHandler) {
        LOGV2_ERROR(31444, "Stack trace failed, symbol handler returned an invalid handle");
        return traceList;
    }

    STACKFRAME64 frame64;
    memset(&frame64, 0, sizeof(frame64));

#if defined(_M_AMD64)
    DWORD imageType = IMAGE_FILE_MACHINE_AMD64;
    frame64.AddrPC.Offset = context.Rip;
    frame64.AddrFrame.Offset = context.Rbp;
    frame64.AddrStack.Offset = context.Rsp;
#elif defined(_M_IX86)
    DWORD imageType = IMAGE_FILE_MACHINE_I386;
    frame64.AddrPC.Offset = context.Eip;
    frame64.AddrFrame.Offset = context.Ebp;
    frame64.AddrStack.Offset = context.Esp;
#else
#error Neither _M_IX86 nor _M_AMD64 were defined
#endif
    frame64.AddrPC.Mode = AddrModeFlat;
    frame64.AddrFrame.Mode = AddrModeFlat;
    frame64.AddrStack.Mode = AddrModeFlat;

    const size_t nameSize = 1024;
    const size_t symbolBufferSize = sizeof(SYMBOL_INFO) + nameSize;
    std::unique_ptr<char[]> symbolCharBuffer(new char[symbolBufferSize]);
    memset(symbolCharBuffer.get(), 0, symbolBufferSize);
    SYMBOL_INFO* symbolBuffer = reinterpret_cast<SYMBOL_INFO*>(symbolCharBuffer.get());
    symbolBuffer->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbolBuffer->MaxNameLen = nameSize;

    for (size_t i = 0; i < kStackTraceFrameMax; ++i) {
        if (!StackWalk64(imageType,
                         symbolHandler.getHandle(),
                         GetCurrentThread(),
                         &frame64,
                         &context,
                         NULL,
                         NULL,
                         NULL,
                         NULL))
            break;
        if (!frame64.AddrReturn.Offset)
            break;
        DWORD64 address = frame64.AddrPC.Offset;
        auto h = symbolHandler.getHandle();
        traceList.push_back({static_cast<uintptr_t>(address),
                             getModuleName(h, address),
                             getSourceLocation(h, address),
                             getSymbolAndOffset(h, address, symbolBuffer)});
    }
    return traceList;
}

void printTraceList(const std::vector<TraceItem>& traceList,
                    StackTraceSink* sink,
                    const Options& options) {
    using namespace fmt::literals;
    if (traceList.empty())
        return;
    BSONObjBuilder bob;
    appendTrace(&bob, traceList, options);
    stack_trace_detail::logBacktraceObject(bob.done(), sink, options.withHumanReadable);
}

/** `sink` can be nullptr to emit structured logs instead of writing to a sink. */
void printWindowsStackTraceImpl(CONTEXT& context, StackTraceSink* sink) {
    Options options{};
    options.withHumanReadable = true;
    options.rawAddress = true;
    printTraceList(makeTraceList(context), sink, options);
}

void printWindowsStackTraceImpl(StackTraceSink* sink) {
    CONTEXT context;
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL;
    RtlCaptureContext(&context);
    printWindowsStackTraceImpl(context, sink);
}

}  // namespace

void printWindowsStackTrace(CONTEXT& context, StackTraceSink& sink) {
    printWindowsStackTraceImpl(context, &sink);
}

void printWindowsStackTrace(CONTEXT& context, std::ostream& os) {
    OstreamStackTraceSink sink{os};
    printWindowsStackTraceImpl(context, &sink);
}

void printWindowsStackTrace(CONTEXT& context) {
    printWindowsStackTraceImpl(context, nullptr);
}

void printStackTrace(StackTraceSink& sink) {
    printWindowsStackTraceImpl(&sink);
}

void printStackTrace(std::ostream& os) {
    OstreamStackTraceSink sink{os};
    printWindowsStackTraceImpl(&sink);
}

void printStackTrace() {
    printWindowsStackTraceImpl(nullptr);
}

}  // namespace mongo
