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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/stacktrace.h"
#include "mongo/util/stacktrace_somap.h"

#include <array>
#include <boost/optional.hpp>
#include <climits>
#include <cstdlib>
#include <dlfcn.h>
#include <iomanip>
#include <iostream>
#include <string>

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/platform/compiler_gcc.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace_json.h"
#include "mongo/util/version.h"

#define MONGO_STACKTRACE_BACKEND_NONE 0
#define MONGO_STACKTRACE_BACKEND_LIBUNWIND 1
#define MONGO_STACKTRACE_BACKEND_EXECINFO 2

#if defined(MONGO_USE_LIBUNWIND)
#define MONGO_STACKTRACE_BACKEND MONGO_STACKTRACE_BACKEND_LIBUNWIND
#elif defined(MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE)
#define MONGO_STACKTRACE_BACKEND MONGO_STACKTRACE_BACKEND_EXECINFO
#else
#define MONGO_STACKTRACE_BACKEND MONGO_STACKTRACE_BACKEND_NONE
#endif

#if MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_LIBUNWIND
#include <libunwind.h>
#elif MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_EXECINFO
#include <execinfo.h>
#endif

namespace mongo {
namespace stack_trace_detail {
namespace {

constexpr size_t kSymbolMax = 512;
constexpr StringData kUnknownFileName = "???"_sd;

// Answer might be negative, but that should be a peculiar case.
ptrdiff_t offsetFromBase(uintptr_t base, uintptr_t addr) {
    return addr - base;
}

struct Options {
    bool withProcessInfo = true;
    bool withHumanReadable = true;
    bool trimSoMap = true;  // only include the somap entries relevant to the backtrace
};


// E.g., for "/foo/bar/my.txt", returns "my.txt".
StringData getBaseName(StringData path) {
    size_t lastSlash = path.rfind('/');
    if (lastSlash == std::string::npos)
        return path;
    return path.substr(lastSlash + 1);
}

class IterationIface {
public:
    enum Flags {
        kRaw = 0,
        kSymbolic = 1,  // Also gather symbolic metadata.
    };

    virtual ~IterationIface() = default;
    virtual void start(Flags f) = 0;
    virtual bool done() const = 0;
    virtual const StackTraceAddressMetadata& deref() const = 0;
    virtual void advance() = 0;
};

/**
 * Iterates through the stacktrace to extract the base for each address in the
 * stacktrace. Returns a sorted, unique sequence of these bases.
 */
size_t uniqueBases(IterationIface& iter, uintptr_t* bases, size_t capacity) {
    auto basesEndMax = bases + capacity;
    auto basesEnd = bases;
    for (iter.start(iter.kSymbolic); basesEnd < basesEndMax && !iter.done(); iter.advance()) {
        const auto& f = iter.deref();
        if (f.file()) {
            // Add the soFile base into bases, keeping it sorted and unique.
            auto base = f.file().base();
            auto position = std::lower_bound(bases, basesEnd, base);
            if (position != basesEnd && *position == base)
                continue;  // skip duplicate base
            *basesEnd++ = base;
            std::rotate(position, basesEnd - 1, basesEnd);
        }
    }
    return basesEnd - bases;
}

void printRawAddrsLine(IterationIface& iter, StackTraceSink& sink, const Options& options) {
    for (iter.start(iter.kRaw); !iter.done(); iter.advance()) {
        sink << " " << Hex(iter.deref().address());
    }
}

void appendJsonBacktrace(IterationIface& iter, CheapJson::Value& jsonRoot) {
    CheapJson::Value frames = jsonRoot.appendKey("backtrace").appendArr();
    for (iter.start(iter.kSymbolic); !iter.done(); iter.advance()) {
        const auto& f = iter.deref();
        auto base = f.file().base();
        CheapJson::Value frameObj = frames.appendObj();
        frameObj.appendKey("b").append(Hex(base));
        frameObj.appendKey("o").append(Hex(offsetFromBase(base, f.address())));
        if (f.symbol()) {
            frameObj.appendKey("s").append(f.symbol().name());
            // We don't write the symbol offset for some reason.
        }
    }
}

/**
 * Most elements of `bsonProcInfo` are copied verbatim into the `jsonProcInfo` Json
 * object. But the "somap" BSON Array is filtered to only include elements corresponding
 * to the addresses contained by the range `[bases, basesEnd)`.
 */
void printJsonProcessInfoTrimmed(const BSONObj& bsonProcInfo,
                                 CheapJson::Value& jsonProcInfo,
                                 const uintptr_t* bases,
                                 const uintptr_t* basesEnd) {
    for (const BSONElement& be : bsonProcInfo) {
        StringData key = be.fieldNameStringData();
        if (be.type() != BSONType::Array || key != "somap"_sd) {
            jsonProcInfo.append(be);
            continue;
        }
        CheapJson::Value jsonSoMap = jsonProcInfo.appendKey(key).appendArr();
        for (const BSONElement& ae : be.Array()) {
            BSONObj bRec = ae.embeddedObject();
            uintptr_t soBase = Hex::fromHex(bRec.getStringField("b"));
            if (std::binary_search(bases, basesEnd, soBase))
                jsonSoMap.append(ae);
        }
    }
}

template <bool isTrimmed>
void appendJsonProcessInfoImpl(IterationIface& iter, CheapJson::Value& jsonRoot) {
    const BSONObj& bsonProcInfo = globalSharedObjectMapInfo().obj();
    CheapJson::Value jsonProcInfo = jsonRoot.appendKey("processInfo").appendObj();
    if constexpr (isTrimmed) {
        uintptr_t bases[kStackTraceFrameMax];
        size_t basesSize = uniqueBases(iter, bases, kStackTraceFrameMax);
        printJsonProcessInfoTrimmed(bsonProcInfo, jsonProcInfo, bases, bases + basesSize);
    } else {
        for (const BSONElement& be : bsonProcInfo) {
            jsonProcInfo.append(be);
        }
    }
}

void appendJsonProcessInfo(IterationIface& iter, CheapJson::Value& jsonRoot, bool trimmed) {
    if (trimmed) {
        appendJsonProcessInfoImpl<true>(iter, jsonRoot);
    } else {
        appendJsonProcessInfoImpl<false>(iter, jsonRoot);
    }
}

void printMetadata(StackTraceSink& sink, const StackTraceAddressMetadata& meta) {
    auto printOffset = [&sink](uintptr_t base, uintptr_t address) {
        ptrdiff_t offset = offsetFromBase(base, address);
        StringData sign = "+"_sd;
        if (offset < 0) {
            sign = "-"_sd;
            offset = -offset;
        }
        sink << sign << Hex(static_cast<uint64_t>(offset), true);
    };

    sink << " ";
    if (meta.file()) {
        sink << getBaseName(meta.file().name());
        sink << "(";
        if (meta.symbol()) {
            sink << meta.symbol().name();
            printOffset(meta.symbol().base(), meta.address());
        } else {
            printOffset(meta.file().base(), meta.address());
        }
        sink << ")";
    } else {
        // Not even shared object information, just punt with unknown filename (SERVER-43551)
        sink << kUnknownFileName;
    }
    sink << " [0x" << Hex(meta.address()) << "]\n";
}

/**
 * Prints a stack backtrace for the current thread to the specified sink.
 *
 * The format of the backtrace is:
 *
 *     hexAddresses ...                    // space-separated
 *     ----- BEGIN BACKTRACE -----
 *     {backtrace:..., processInfo:...}    // json
 *     Human-readable backtrace
 *     -----  END BACKTRACE  -----
 *
 * The JSON backtrace will be a JSON object with a "backtrace" field, and optionally others.
 * The "backtrace" field is an array, whose elements are frame objects.  A frame object has a
 * "b" field, which is the base-address of the library or executable containing the symbol, and
 * an "o" field, which is the offset into said library or executable of the symbol.
 *
 * The JSON backtrace may optionally contain additional information useful to a backtrace
 * analysis tool.  For example, on Linux it contains a subobject named "somap", describing
 * the objects referenced in the "b" fields of the "backtrace" list.
 */
void printStackTraceGeneric(StackTraceSink& sink, IterationIface& iter, const Options& options) {
    printRawAddrsLine(iter, sink, options);
    sink << "\n----- BEGIN BACKTRACE -----\n";
    {
        CheapJson json{sink};
        CheapJson::Value doc = json.doc();
        CheapJson::Value jsonRootObj = doc.appendObj();
        appendJsonBacktrace(iter, jsonRootObj);
        if (options.withProcessInfo) {
            appendJsonProcessInfo(iter, jsonRootObj, options.trimSoMap);
        }
    }
    sink << "\n";
    if (options.withHumanReadable) {
        for (iter.start(iter.kSymbolic); !iter.done(); iter.advance()) {
            printMetadata(sink, iter.deref());
        }
    }
    sink << "-----  END BACKTRACE  -----\n";
}

void mergeDlInfo(StackTraceAddressMetadata& f) {
    Dl_info dli;
    // `man dladdr`:
    //   On success, these functions return a nonzero value.  If the address
    //   specified in addr could be matched to a shared object, but not to a
    //   symbol in the shared object, then the info->dli_sname and
    //   info->dli_saddr fields are set to NULL.
    if (dladdr(reinterpret_cast<void*>(f.address()), &dli) == 0) {
        return;  // f.address doesn't map to a shared object
    }
    if (!f.file() && dli.dli_fbase) {
        f.file().assign(reinterpret_cast<uintptr_t>(dli.dli_fbase), dli.dli_fname);
    }
    if (!f.symbol() && dli.dli_saddr) {
        f.symbol().assign(reinterpret_cast<uintptr_t>(dli.dli_saddr), dli.dli_sname);
    }
}

#if MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_LIBUNWIND

class LibunwindStepIteration : public IterationIface {
public:
    explicit LibunwindStepIteration(StackTraceSink& sink) : _sink(sink) {
        if (int r = unw_getcontext(&_context); r < 0) {
            _sink << "unw_getcontext: " << unw_strerror(r) << "\n";
            _failed = true;
        }
    }

private:
    void start(Flags flags) override {
        _flags = flags;
        _end = false;

        if (_failed) {
            _end = true;
            return;
        }
        int r = unw_init_local(&_cursor, &_context);
        if (r < 0) {
            _sink << "unw_init_local: " << unw_strerror(r) << "\n";
            _end = true;
            return;
        }
        _load();
    }

    bool done() const override {
        return _end;
    }

    const StackTraceAddressMetadata& deref() const override {
        return _meta;
    }

    void advance() override {
        int r = unw_step(&_cursor);
        if (r <= 0) {
            if (r < 0) {
                _sink << "error: unw_step: " << unw_strerror(r) << "\n";
            }
            _end = true;
        }
        if (!_end) {
            _load();
        }
    }

    void _load() {
        unw_word_t pc;
        if (int r = unw_get_reg(&_cursor, UNW_REG_IP, &pc); r < 0) {
            _sink << "unw_get_reg: " << unw_strerror(r) << "\n";
            _end = true;
            return;
        }
        if (pc == 0) {
            _end = true;
            return;
        }
        _meta.reset(static_cast<uintptr_t>(pc));
        if (_flags & kSymbolic) {
            // `unw_get_proc_name`, with its acccess to a cursor, and to libunwind's
            // dwarf reader, can generate better metadata than mergeDlInfo, so prefer it.
            unw_word_t offset;
            if (int r = unw_get_proc_name(&_cursor, _symbolBuf, sizeof(_symbolBuf), &offset);
                r < 0) {
                _sink << "unw_get_proc_name(" << Hex(_meta.address()) << "): " << unw_strerror(r)
                      << "\n";
            } else {
                _meta.symbol().assign(_meta.address() - offset, _symbolBuf);
            }
            mergeDlInfo(_meta);
        }
    }

    StackTraceSink& _sink;

    Flags _flags;
    StackTraceAddressMetadata _meta;

    bool _failed = false;
    bool _end = false;

    unw_context_t _context;
    unw_cursor_t _cursor;

    char _symbolBuf[kSymbolMax];
};
#endif  // MONGO_STACKTRACE_BACKEND

class RawBacktraceIteration : public IterationIface {
public:
    explicit RawBacktraceIteration(StackTraceSink& sink) {
        _n = rawBacktrace(_addresses.data(), _addresses.size());
        if (_n == 0) {
            int err = errno;
            sink << "Unable to collect backtrace addresses (errno: " << Dec(err) << " "
                 << strerror(err) << ")\n";
            return;
        }
    }

private:
    void start(Flags flags) override {
        _flags = flags;
        _i = 0;
        if (!done())
            _load();
    }

    bool done() const override {
        return _i >= _n;
    }

    const StackTraceAddressMetadata& deref() const override {
        return _meta;
    }

    void advance() override {
        ++_i;
        if (!done())
            _load();
    }

    void _load() {
        _meta.reset(reinterpret_cast<uintptr_t>(_addresses[_i]));
        if (_flags & kSymbolic) {
            mergeDlInfo(_meta);
        }
    }

    Flags _flags;
    StackTraceAddressMetadata _meta;

    std::array<void*, kStackTraceFrameMax> _addresses;
    size_t _n = 0;
    size_t _i = 0;
};

}  // namespace
}  // namespace stack_trace_detail

void StackTraceAddressMetadata::printTo(StackTraceSink& sink) const {
    stack_trace_detail::printMetadata(sink, *this);
}

size_t rawBacktrace(void** addrs, size_t capacity) {
#if MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_LIBUNWIND
    return ::unw_backtrace(addrs, capacity);
#elif MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_EXECINFO
    return ::backtrace(addrs, capacity);
#elif MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_NONE
    return 0;
#endif
}

const StackTraceAddressMetadata& StackTraceAddressMetadataGenerator::load(void* address) {
    _meta.reset(reinterpret_cast<uintptr_t>(address));
    stack_trace_detail::mergeDlInfo(_meta);
    return _meta;
}

void printStackTrace(StackTraceSink& sink) {
#if MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_LIBUNWIND
    stack_trace_detail::Options options{};
    static constexpr bool kUseUnwindSteps = false;
    if (kUseUnwindSteps) {
        stack_trace_detail::LibunwindStepIteration iteration(sink);
        printStackTraceGeneric(sink, iteration, options);
    } else {
        stack_trace_detail::RawBacktraceIteration iteration(sink);
        printStackTraceGeneric(sink, iteration, options);
    }
#elif MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_EXECINFO
    stack_trace_detail::Options options{};
    stack_trace_detail::RawBacktraceIteration iteration(sink);
    printStackTraceGeneric(sink, iteration, options);
#elif MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_NONE
    sink << "This platform does not support printing stacktraces\n";
#endif
}

void printStackTrace(std::ostream& os) {
    OstreamStackTraceSink sink{os};
    printStackTrace(sink);
}

void printStackTrace() {
    // NOTE: We disable long-line truncation for the stack trace, because the JSON
    // representation of the stack trace can sometimes exceed the long line limit.
    printStackTrace(log().setIsTruncatable(false).stream());
}

}  // namespace mongo
