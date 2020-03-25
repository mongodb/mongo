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

#include <array>
#include <boost/optional.hpp>
#include <climits>
#include <cstdlib>
#include <dlfcn.h>
#include <iomanip>
#include <iostream>
#include <string>

#include "mongo/base/init.h"
#include "mongo/bson/json.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler_gcc.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stacktrace_somap.h"
#include "mongo/util/version.h"

#define MONGO_STACKTRACE_BACKEND_NONE 0
#define MONGO_STACKTRACE_BACKEND_LIBUNWIND 1
#define MONGO_STACKTRACE_BACKEND_EXECINFO 2

#if defined(MONGO_CONFIG_USE_LIBUNWIND)
#define MONGO_STACKTRACE_BACKEND MONGO_STACKTRACE_BACKEND_LIBUNWIND
#elif defined(MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE)
#define MONGO_STACKTRACE_BACKEND MONGO_STACKTRACE_BACKEND_EXECINFO
#else
#define MONGO_STACKTRACE_BACKEND MONGO_STACKTRACE_BACKEND_NONE
#endif

#if MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_LIBUNWIND
#define UNW_LOCAL_ONLY
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
    // Add the processInfo block
    bool withProcessInfo = true;

    // Add "human readable" breakdown when dumping stack. 1 line per frame.
    bool withHumanReadable = true;

    // only include the somap entries relevant to the backtrace
    bool trimSoMap = true;

    // include "a" field with raw addr (use for fatal traces only)
    bool rawAddress = false;
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
std::vector<uintptr_t> uniqueBases(IterationIface& iter, size_t capacity) {
    std::vector<uintptr_t> bases;
    for (iter.start(iter.kSymbolic); bases.size() < capacity && !iter.done(); iter.advance()) {
        const auto& f = iter.deref();
        if (!f.file())
            continue;
        // Add the soFile base into bases, keeping it sorted and unique.
        auto base = f.file().base();
        auto position = std::lower_bound(bases.begin(), bases.end(), base);
        if (position != bases.end() && *position == base)
            continue;  // skip duplicate base
        bases.insert(position, base);
    }
    return bases;
}

void appendBacktrace(BSONObjBuilder* obj, IterationIface& iter, const Options& options) {
    BSONArrayBuilder frames(obj->subarrayStart("backtrace"));
    for (iter.start(iter.kSymbolic); !iter.done(); iter.advance()) {
        const auto& meta = iter.deref();
        const uintptr_t addr = reinterpret_cast<uintptr_t>(meta.address());
        BSONObjBuilder frame(frames.subobjStart());
        if (options.rawAddress) {
            frame.append("a", Hex(addr));
        }
        if (const auto& mf = meta.file(); mf) {
            frame.append("b", Hex(mf.base()));
            frame.append("o", Hex(offsetFromBase(mf.base(), addr)));
        }
        if (const auto& sym = meta.symbol(); sym) {
            frame.append("s", sym.name());
            frame.append("s+", Hex(offsetFromBase(sym.base(), addr)));
        }
    }
}

/**
 * Most elements of `bsonProcInfo` are copied verbatim into the `jsonProcInfo` Json
 * object. But the "somap" BSON Array is filtered to only include elements corresponding
 * to the addresses contained by `bases`.
 */
void appendProcessInfoTrimmed(const BSONObj& bsonProcInfo,
                              const std::vector<uintptr_t>& bases,
                              BSONObjBuilder* bob) {
    for (const BSONElement& be : bsonProcInfo) {
        StringData key = be.fieldNameStringData();
        if (be.type() != BSONType::Array || key != "somap"_sd) {
            bob->append(be);
            continue;
        }
        BSONArrayBuilder so(bob->subarrayStart(key));
        for (const BSONElement& ae : be.Array()) {
            BSONObj bRec = ae.embeddedObject();
            uintptr_t soBase = Hex::fromHex(bRec.getStringField("b"));
            if (std::binary_search(bases.begin(), bases.end(), soBase))
                so.append(ae);
        }
    }
}

void appendStackTraceObject(BSONObjBuilder* obj, IterationIface& iter, const Options& options) {
    appendBacktrace(obj, iter, options);
    if (options.withProcessInfo) {
        const BSONObj& bsonProcInfo = globalSharedObjectMapInfo().obj();
        BSONObjBuilder bob(obj->subobjStart("processInfo"));
        if (options.trimSoMap) {
            appendProcessInfoTrimmed(bsonProcInfo, uniqueBases(iter, kStackTraceFrameMax), &bob);
        } else {
            for (const BSONElement& be : bsonProcInfo) {
                bob.append(be);
            }
        }
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
        return (_i == kStackTraceFrameMax) || _end;
    }

    const StackTraceAddressMetadata& deref() const override {
        return _meta;
    }

    void advance() override {
        ++_i;
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
            // `unw_get_proc_name`, with its access to a cursor, and to libunwind's
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
    size_t _i = 0;

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

/**
 * Prints a stack backtrace for the current thread to the specified sink.
 * @param sink sink to print to, or print to LOGV2 if sink is nullptr.
 *
 * The format of the backtrace is:
 *
 *     {
 *      backtrace: [...],
 *      processInfo: {...}
 *     }
 *
 * The backtrace will be a JSON object with a "backtrace" field, and optionally others.
 * The "backtrace" field is an array of frame objects. A frame object has a
 * "b" field, which is the base-address of the library or executable containing the symbol, and
 * an "o" field, which is the offset into said library or executable of the symbol.
 *
 * The backtrace may optionally contain additional information useful to a backtrace
 * analysis tool. For example, on Linux it contains a subobject named "somap", describing
 * the objects referenced in the "b" fields of the "backtrace" list.
 */
void printStackTraceImpl(const Options& options, StackTraceSink* sink = nullptr) {
    using namespace fmt::literals;
    std::string err;
    BSONObjBuilder bob;
#if (MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_NONE)
    err = "This platform does not support printing stacktraces";
#else
#if (MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_LIBUNWIND)
    using IterationType = LibunwindStepIteration;
#else
    using IterationType = RawBacktraceIteration;
#endif
    StringStackTraceSink errSink{err};
    IterationType iteration(errSink);
    appendStackTraceObject(&bob, iteration, options);
#endif

    if (!err.empty()) {
        if (sink) {
            *sink << fmt::format(FMT_STRING("Error collecting stack trace: {}"), err);
        } else {
            LOGV2(31430, "Error collecting stack trace", "error"_attr = err);
        }
        return;
    }
    stack_trace_detail::logBacktraceObject(bob.done(), sink, options.withHumanReadable);
}


}  // namespace
}  // namespace stack_trace_detail

void StackTraceAddressMetadata::printTo(StackTraceSink& sink) const {
    stack_trace_detail::printMetadata(sink, *this);
}

size_t rawBacktrace(void** addrs, size_t capacity) {
#if (MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_LIBUNWIND)
    return ::unw_backtrace(addrs, capacity);
#elif (MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_EXECINFO)
    return ::backtrace(addrs, capacity);
#else
    return 0;
#endif
}

const StackTraceAddressMetadata& StackTraceAddressMetadataGenerator::load(void* address) {
    _meta.reset(reinterpret_cast<uintptr_t>(address));
    stack_trace_detail::mergeDlInfo(_meta);
    return _meta;
}

void printStackTrace(StackTraceSink& sink) {
    stack_trace_detail::Options options{};
    options.rawAddress = true;
    stack_trace_detail::printStackTraceImpl(options, &sink);
}

void printStackTrace(std::ostream& os) {
    OstreamStackTraceSink sink{os};
    printStackTrace(sink);
}

void printStackTrace() {
    stack_trace_detail::Options options{};
    options.rawAddress = true;
    stack_trace_detail::printStackTraceImpl(options, nullptr);
}

}  // namespace mongo
