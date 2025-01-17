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


#include <dlfcn.h>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
// IWYU pragma: no_include "libunwind-x86_64.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/util/hex.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/stacktrace_somap.h"

#define MONGO_STACKTRACE_BACKEND_NONE 0
#define MONGO_STACKTRACE_BACKEND_LIBUNWIND 1
#define MONGO_STACKTRACE_BACKEND_EXECINFO 2

#if defined(MONGO_CONFIG_USE_LIBUNWIND)

#if __has_feature(thread_sanitizer)
// TODO: SERVER-48622 (and see also https://github.com/google/sanitizers/issues/943)
#error "Cannot currently use libunwind with -fsanitize=thread"
#endif

#define MONGO_STACKTRACE_BACKEND MONGO_STACKTRACE_BACKEND_LIBUNWIND
#elif defined(MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE)
#define MONGO_STACKTRACE_BACKEND MONGO_STACKTRACE_BACKEND_EXECINFO
#else
#define MONGO_STACKTRACE_BACKEND MONGO_STACKTRACE_BACKEND_NONE
#endif

#if MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_LIBUNWIND
#define UNW_LOCAL_ONLY
#include <elf.h>
#include <libunwind.h>
#include <link.h>
#elif MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_EXECINFO
#include <execinfo.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


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
    virtual ~IterationIface() = default;
    virtual void start() = 0;
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
    for (iter.start(); bases.size() < capacity && !iter.done(); iter.advance()) {
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
    for (iter.start(); !iter.done(); iter.advance()) {
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
            int status;
            char* realname = abi::__cxa_demangle(std::string(sym.name()).c_str(), 0, 0, &status);
            if (status == 0)
                frame.append("C", realname);
            std::free(realname);
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
        // libunwind assumes that register/state capture (unw_getcontext) and stack unwinding
        // (unw_step) happen in the same or child frames.
        unw_context_t context;

        if (int r = unw_getcontext(&context); r < 0) {
            _sink << "unw_getcontext: " << unw_strerror(r) << "\n";
            return;
        }

        captureStackFrames(&context);
    }

private:
    void captureStackFrames(unw_context_t* context) {
        unw_cursor_t cursor;

        _metas.reserve(kStackTraceFrameMax);

        if (int r = unw_init_local(&cursor, context); r < 0) {
            _sink << "unw_init_local: " << unw_strerror(r) << "\n";
            return;
        }

        while (_metas.size() < kStackTraceFrameMax) {
            if (int r = unw_step(&cursor); r <= 0) {
                if (r < 0) {
                    _sink << "error: unw_step: " << unw_strerror(r) << "\n";
                }

                return;
            }

            unw_word_t pc;
            if (int r = unw_get_reg(&cursor, UNW_REG_IP, &pc); r < 0) {
                _sink << "unw_get_reg: " << unw_strerror(r) << "\n";
                return;
            }

            if (pc == 0) {
                return;
            }

            _metas.emplace_back();
            StackTraceAddressMetadata& meta = _metas.back();

            meta.reset(static_cast<uintptr_t>(pc));

            // `unw_get_proc_name`, with its access to a cursor, and to libunwind's
            // dwarf reader, can generate better metadata than mergeDlInfo, so prefer it.
            unw_word_t offset;
            if (int r = unw_get_proc_name(&cursor, _symbolBuf, sizeof(_symbolBuf), &offset);
                r < 0) {
                _sink << "unw_get_proc_name(" << Hex(meta.address()) << "): " << unw_strerror(r)
                      << "\n";
            } else {
                meta.symbol().assign(meta.address() - offset, _symbolBuf);
            }

            mergeDlInfo(meta);
        }
    }

    void start() override {
        _i = _metas.begin();
    }

    bool done() const override {
        return _i == _metas.end();
    }

    const StackTraceAddressMetadata& deref() const override {
        return *_i;
    }

    void advance() override {
        if (_i != _metas.end()) {
            ++_i;
        }
    }

    StackTraceSink& _sink;
    std::vector<StackTraceAddressMetadata> _metas;
    decltype(_metas)::iterator _i;

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
    void start() override {
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
        mergeDlInfo(_meta);
    }

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
StackTrace getStackTraceImpl(const Options& options) {
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

    return StackTrace(bob.obj(), err);
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

StackTrace getStackTrace() {
    stack_trace_detail::Options options{};
    options.rawAddress = true;
    return getStackTraceImpl(options);
}

void printStackTrace(StackTraceSink& sink) {
    stack_trace_detail::Options options{};
    options.rawAddress = true;
    const bool withHumanReadable = true;
    getStackTraceImpl(options).sink(&sink, withHumanReadable);
}

void printStackTrace(std::ostream& os) {
    OstreamStackTraceSink sink{os};
    printStackTrace(sink);
}

void printStackTrace() {
    stack_trace_detail::Options options{};
    options.rawAddress = true;
    const bool withHumanReadable = true;
    getStackTraceImpl(options).log(withHumanReadable);
}

#if defined(MONGO_CONFIG_USE_LIBUNWIND)
namespace {

/**
 * Stashes the results of a `dl_iterate_phdr`
 * so that it can be accessed in signal handlers.
 */
class DlPhdrStore {
public:
    int initCb(dl_phdr_info* info, size_t sz) {
        _entries.push_back(Entry{
            .addr = info->dlpi_addr,
            .name = std::string{info->dlpi_name},
            .phdr = info->dlpi_phdr,
            .phnum = info->dlpi_phnum,
            .adds = info->dlpi_adds,
            .subs = info->dlpi_subs,
            .tls_modid = info->dlpi_tls_modid,
            .tls_data = info->dlpi_tls_data,
        });
        return 0;
    }

    int iterate(unw_iterate_phdr_callback_t cb, void* cbData) {
        for (auto&& e : _entries) {
            dl_phdr_info info{
                .dlpi_addr = e.addr,
                .dlpi_name = e.name.c_str(),
                .dlpi_phdr = e.phdr,
                .dlpi_phnum = e.phnum,
                .dlpi_adds = e.adds,
                .dlpi_subs = e.subs,
                .dlpi_tls_modid = e.tls_modid,
                .dlpi_tls_data = e.tls_data,
            };
            if (int r = cb(&info, sizeof(info), cbData))
                return r;
        }
        return 0;
    }

    void initialize();

private:
    struct Entry {
        ElfW(Addr) addr;
        std::string name;
        const ElfW(Phdr) * phdr;
        ElfW(Half) phnum;
        unsigned long long int adds;
        unsigned long long int subs;
        size_t tls_modid;
        void* tls_data;
    };

    std::vector<Entry> _entries;
};

DlPhdrStore dlPhdrStore;

extern "C" int initCb_C(dl_phdr_info* info, size_t sz, void* cbData) {
    auto store = static_cast<DlPhdrStore*>(cbData);
    invariant(store == &dlPhdrStore);
    return store->initCb(info, sz);
}

extern "C" int iterateCb_C(unw_iterate_phdr_callback_t cb, void* cbData) {
    return dlPhdrStore.iterate(cb, cbData);
}

void DlPhdrStore::initialize() {
    dl_iterate_phdr(&initCb_C, this);
    unw_set_iterate_phdr_function(unw_local_addr_space, &iterateCb_C);

    for (auto&& e : _entries) {
        LOGV2_DEBUG(9077500,
                    5,
                    "Preloaded dl_iterate_phdr entry",
                    "name"_attr = e.name,
                    "addr"_attr = unsignedHex(e.addr));
    }
}

MONGO_INITIALIZER(LibunwindPrefetch)(InitializerContext*) {
    dlPhdrStore.initialize();
}

}  // namespace
#endif  // MONGO_CONFIG_USE_LIBUNWIND

}  // namespace mongo
