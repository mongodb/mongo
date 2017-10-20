/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

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
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/init.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/text.h"

namespace mongo {
namespace {
const auto kPathBufferSize = 1024;

// On Windows the symbol handler must be initialized at process startup and cleaned up at shutdown.
// This class wraps up that logic and gives access to the process handle associated with the
// symbol handler. Because access to the symbol handler API is not thread-safe, it also provides
// a lock/unlock method so the whole symbol handler can be used with a stdx::lock_guard.
class SymbolHandler {
    MONGO_DISALLOW_COPYING(SymbolHandler);

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

        BOOL ret = SymInitializeW(handle, symbolPath.c_str(), TRUE);
        if (ret == FALSE) {
            error() << "Stack trace initialization failed, SymInitialize failed with error "
                    << errnoWithDescription();
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
    stdx::mutex _mutex;
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

}  // namespace

/**
 * Get the display name of the executable module containing the specified address.
 *
 * @param process               Process handle
 * @param address               Address to find
 * @param returnedModuleName    Returned module name
 */
static void getModuleName(HANDLE process, DWORD64 address, std::string* returnedModuleName) {
    IMAGEHLP_MODULE64 module64;
    memset(&module64, 0, sizeof(module64));
    module64.SizeOfStruct = sizeof(module64);
    BOOL ret = SymGetModuleInfo64(process, address, &module64);
    if (FALSE == ret) {
        returnedModuleName->clear();
        return;
    }
    char* moduleName = module64.LoadedImageName;
    char* backslash = strrchr(moduleName, '\\');
    if (backslash) {
        moduleName = backslash + 1;
    }
    *returnedModuleName = moduleName;
}

/**
 * Get the display name and line number of the source file containing the specified address.
 *
 * @param process               Process handle
 * @param address               Address to find
 * @param returnedSourceAndLine Returned source code file name with line number
 */
static void getSourceFileAndLineNumber(HANDLE process,
                                       DWORD64 address,
                                       std::string* returnedSourceAndLine) {
    IMAGEHLP_LINE64 line64;
    memset(&line64, 0, sizeof(line64));
    line64.SizeOfStruct = sizeof(line64);
    DWORD displacement32;
    BOOL ret = SymGetLineFromAddr64(process, address, &displacement32, &line64);
    if (FALSE == ret) {
        returnedSourceAndLine->clear();
        return;
    }

    std::string filename(line64.FileName);
    std::string::size_type start = filename.find("\\src\\mongo\\");
    if (start == std::string::npos) {
        start = filename.find("\\src\\third_party\\");
    }
    if (start != std::string::npos) {
        std::string shorter("...");
        shorter += filename.substr(start);
        filename.swap(shorter);
    }
    static const size_t bufferSize = 32;
    std::unique_ptr<char[]> lineNumber(new char[bufferSize]);
    _snprintf(lineNumber.get(), bufferSize, "(%u)", line64.LineNumber);
    filename += lineNumber.get();
    returnedSourceAndLine->swap(filename);
}

/**
 * Get the display text of the symbol and offset of the specified address.
 *
 * @param process                   Process handle
 * @param address                   Address to find
 * @param symbolInfo                Caller's pre-built SYMBOL_INFO struct (for efficiency)
 * @param returnedSymbolAndOffset   Returned symbol and offset
 */
static void getsymbolAndOffset(HANDLE process,
                               DWORD64 address,
                               SYMBOL_INFO* symbolInfo,
                               std::string* returnedSymbolAndOffset) {
    DWORD64 displacement64;
    BOOL ret = SymFromAddr(process, address, &displacement64, symbolInfo);
    if (FALSE == ret) {
        *returnedSymbolAndOffset = "???";
        return;
    }
    std::string symbolString(symbolInfo->Name);
    static const size_t bufferSize = 32;
    std::unique_ptr<char[]> symbolOffset(new char[bufferSize]);
    _snprintf(symbolOffset.get(), bufferSize, "+0x%llux", displacement64);
    symbolString += symbolOffset.get();
    returnedSymbolAndOffset->swap(symbolString);
}

struct TraceItem {
    std::string moduleName;
    std::string sourceAndLine;
    std::string symbolAndOffset;
};

static const int maxBackTraceFrames = 100;

/**
 * Print a stack backtrace for the current thread to the specified ostream.
 *
 * @param os    ostream& to receive printed stack backtrace
 */
void printStackTrace(std::ostream& os) {
    CONTEXT context;
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL;
    RtlCaptureContext(&context);
    printWindowsStackTrace(context, os);
}


/**
 * Print stack trace (using a specified stack context) to "os"
 *
 * @param context   CONTEXT record for stack trace
 * @param os        ostream& to receive printed stack backtrace
 */
void printWindowsStackTrace(CONTEXT& context, std::ostream& os) {
    auto& symbolHandler = SymbolHandler::instance();
    stdx::lock_guard<SymbolHandler> lk(symbolHandler);

    if (!symbolHandler) {
        error() << "Stack trace failed, symbol handler returned an invalid handle.";
        return;
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

    // build list
    std::vector<TraceItem> traceList;
    TraceItem traceItem;
    size_t moduleWidth = 0;
    size_t sourceWidth = 0;
    for (size_t i = 0; i < maxBackTraceFrames; ++i) {
        BOOL ret = StackWalk64(imageType,
                               symbolHandler.getHandle(),
                               GetCurrentThread(),
                               &frame64,
                               &context,
                               NULL,
                               NULL,
                               NULL,
                               NULL);
        if (ret == FALSE || frame64.AddrReturn.Offset == 0) {
            break;
        }
        DWORD64 address = frame64.AddrPC.Offset;
        getModuleName(symbolHandler.getHandle(), address, &traceItem.moduleName);
        size_t width = traceItem.moduleName.length();
        if (width > moduleWidth) {
            moduleWidth = width;
        }
        getSourceFileAndLineNumber(symbolHandler.getHandle(), address, &traceItem.sourceAndLine);
        width = traceItem.sourceAndLine.length();
        if (width > sourceWidth) {
            sourceWidth = width;
        }
        getsymbolAndOffset(
            symbolHandler.getHandle(), address, symbolBuffer, &traceItem.symbolAndOffset);
        traceList.push_back(traceItem);
    }

    // print list
    ++moduleWidth;
    ++sourceWidth;
    size_t frameCount = traceList.size();
    for (size_t i = 0; i < frameCount; ++i) {
        std::stringstream ss;
        ss << traceList[i].moduleName << " ";
        size_t width = traceList[i].moduleName.length();
        while (width < moduleWidth) {
            ss << " ";
            ++width;
        }
        ss << traceList[i].sourceAndLine << " ";
        width = traceList[i].sourceAndLine.length();
        while (width < sourceWidth) {
            ss << " ";
            ++width;
        }
        ss << traceList[i].symbolAndOffset;
        log() << ss.str();
    }
}

// Print error message from C runtime, then fassert
int crtDebugCallback(int, char* originalMessage, int*) {
    StringData message(originalMessage);
    log() << "*** C runtime error: " << message.substr(0, message.find('\n')) << ", terminating";
    fassertFailed(17006);
}
}
