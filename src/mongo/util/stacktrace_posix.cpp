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
#include "mongo/util/version.h"

#define MONGO_STACKTRACE_BACKEND_LIBUNWIND 1
#define MONGO_STACKTRACE_BACKEND_EXECINFO 2
#define MONGO_STACKTRACE_BACKEND_NONE 3

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
namespace stacktrace_detail {

constexpr int kFrameMax = 100;
constexpr size_t kSymbolMax = 512;
constexpr StringData kUnknownFileName = "???"_sd;

struct StackTraceOptions {
    bool withProcessInfo = true;
    bool withHumanReadable = true;
};

// E.g., for "/foo/bar/my.txt", returns "my.txt".
StringData getBaseName(StringData path) {
    size_t lastSlash = path.rfind('/');
    if (lastSlash == std::string::npos)
        return path;
    return path.substr(lastSlash + 1);
}

struct NameBase {
    StringData name;
    uintptr_t base;
};

// Metadata about an instruction address.
// Beyond that, it may have an enclosing shared object file.
// Further, it may have an enclosing symbol within that shared object.
struct AddressMetadata {
    uintptr_t address{};
    boost::optional<NameBase> soFile{};
    boost::optional<NameBase> symbol{};
};

class IterationIface {
public:
    enum Flags {
        kRaw = 0,
        kSymbolic = 1,  // Also gather symbolic metadata.
    };

    virtual ~IterationIface() = default;
    virtual void start(Flags f) = 0;
    virtual bool done() const = 0;
    virtual const AddressMetadata& deref() const = 0;
    virtual void advance() = 0;
};

template <typename T>
struct Quoted {
    explicit Quoted(const T& v) : v(v) {}

    friend std::ostream& operator<<(std::ostream& os, const Quoted& q) {
        return os << "\"" << q.v << "\"";
    }

    const T& v;
};

/**
 * Prints a stack backtrace for the current thread to the specified ostream.
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
void printStackTraceGeneric(IterationIface& source,
                            std::ostream& os,
                            const StackTraceOptions& options) {
    // TODO: make this signal-safe.
    os << std::hex << std::uppercase;
    auto sg = makeGuard([&] { os << std::dec << std::nouppercase; });
    // First, just the raw backtrace addresses.
    for (source.start(source.kRaw); !source.done(); source.advance()) {
        const auto& f = source.deref();
        os << ' ' << f.address;
    }

    os << "\n----- BEGIN BACKTRACE -----\n";

    // Display the JSON backtrace
    {
        os << "{";
        os << Quoted("backtrace") << ":";
        {
            os << "[";
            StringData frameComma;
            for (source.start(source.kSymbolic); !source.done(); source.advance()) {
                const auto& f = source.deref();
                os << frameComma;
                frameComma = ","_sd;
                {
                    os << "{";
                    StringData fieldSep = "";
                    auto addField = [&](StringData k, auto&& v) {
                        os << fieldSep << Quoted(k) << ":" << Quoted(v);
                        fieldSep = ","_sd;
                    };
                    if (f.soFile) {
                        // Cast because integers obey `uppercase` and pointers don't.
                        addField("b", f.soFile->base);
                        addField("o", (f.address - f.soFile->base));
                        if (f.symbol) {
                            addField("s", f.symbol->name);
                        }
                    } else {
                        addField("b", uintptr_t{0});
                        addField("o", f.address);
                    }
                    os << "}";
                }
            }
            os << "]";
        }
        if (options.withProcessInfo) {
            if (auto soMap = globalSharedObjectMapInfo())
                os << "," << Quoted("processInfo") << ":" << soMap->json();
        }
        os << "}";
    }
    os << "\n";
    // Display the human-readable trace
    if (options.withHumanReadable) {
        for (source.start(source.kSymbolic); !source.done(); source.advance()) {
            const auto& f = source.deref();
            os << " ";
            if (f.soFile) {
                os << getBaseName(f.soFile->name);
                os << "(";
                if (f.symbol) {
                    os << f.symbol->name << "+0x" << (f.address - f.symbol->base);
                } else {
                    // No symbol, so fall back to the `soFile` offset.
                    os << "+0x" << (f.address - f.soFile->base);
                }
                os << ")";
            } else {
                // Not even shared object information, just punt with unknown filename.
                os << kUnknownFileName;
            }
            os << " [0x" << f.address << "]\n";
        }
    }
    os << "-----  END BACKTRACE  -----\n";
}

void mergeDlInfo(AddressMetadata& f) {
    Dl_info dli;
    // `man dladdr`:
    //   On success, these functions return a nonzero value.  If the address
    //   specified in addr could be matched to a shared object, but not to a
    //   symbol in the shared object, then the info->dli_sname and
    //   info->dli_saddr fields are set to NULL.
    if (dladdr(reinterpret_cast<void*>(f.address), &dli) == 0) {
        return;  // f.address doesn't map to a shared object
    }
    if (!f.soFile) {
        f.soFile = NameBase{dli.dli_fname, reinterpret_cast<uintptr_t>(dli.dli_fbase)};
    }
    if (!f.symbol) {
        if (dli.dli_saddr) {
            // matched to a symbol in the shared object
            f.symbol = NameBase{dli.dli_sname, reinterpret_cast<uintptr_t>(dli.dli_saddr)};
        }
    }
}

#if MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_LIBUNWIND

class Iteration : public IterationIface {
public:
    explicit Iteration(std::ostream& os, bool fromSignal) : _os(os), _fromSignal(fromSignal) {
        if (int r = unw_getcontext(&_context); r < 0) {
            _os << "unw_getcontext: " << unw_strerror(r) << std::endl;
            _failed = true;
        }
    }

private:
    void start(Flags f) override {
        _flags = f;
        _end = false;

        if (_failed) {
            _end = true;
            return;
        }
        int r = unw_init_local2(&_cursor, &_context, _fromSignal ? UNW_INIT_SIGNAL_FRAME : 0);
        if (r < 0) {
            _os << "unw_init_local2: " << unw_strerror(r) << std::endl;
            _end = true;
            return;
        }
        _load();
    }

    bool done() const override {
        return _end;
    }

    const AddressMetadata& deref() const override {
        return _f;
    }

    void advance() override {
        int r = unw_step(&_cursor);
        if (r <= 0) {
            if (r < 0) {
                _os << "error: unw_step: " << unw_strerror(r) << std::endl;
            }
            _end = true;
        }
        if (!_end) {
            _load();
        }
    }

    void _load() {
        _f = {};
        unw_word_t pc;
        if (int r = unw_get_reg(&_cursor, UNW_REG_IP, &pc); r < 0) {
            _os << "unw_get_reg: " << unw_strerror(r) << std::endl;
            _end = true;
            return;
        }
        if (pc == 0) {
            _end = true;
            return;
        }
        _f.address = pc;
        if (_flags & kSymbolic) {
            unw_word_t offset;
            if (int r = unw_get_proc_name(&_cursor, _symbolBuf, sizeof(_symbolBuf), &offset);
                r < 0) {
                _os << "unw_get_proc_name(" << _f.address << "): " << unw_strerror(r) << std::endl;
            } else {
                _f.symbol = NameBase{_symbolBuf, _f.address - offset};
            }
            mergeDlInfo(_f);
        }
    }

    std::ostream& _os;
    bool _fromSignal;

    Flags _flags;
    AddressMetadata _f{};

    bool _failed = false;
    bool _end = false;

    unw_context_t _context;
    unw_cursor_t _cursor;

    char _symbolBuf[kSymbolMax];
};

MONGO_COMPILER_NOINLINE
void printStackTrace(std::ostream& os, bool fromSignal) {
    Iteration iteration(os, fromSignal);
    printStackTraceGeneric(iteration, os, StackTraceOptions{});
}

#elif MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_EXECINFO

class Iteration : public IterationIface {
public:
    explicit Iteration(std::ostream& os, bool fromSignal) {
        _n = ::backtrace(_addresses.data(), _addresses.size());
        if (_n == 0) {
            int err = errno;
            os << "Unable to collect backtrace addresses (errno: " << err << ' ' << strerror(err)
               << ')' << std::endl;
            return;
        }
    }

private:
    void start(Flags f) override {
        _flags = f;
        _i = 0;
        if (!done())
            _load();
    }
    bool done() const override {
        return _i >= _n;
    }
    const AddressMetadata& deref() const override {
        return _f;
    }
    void advance() override {
        ++_i;
        if (!done())
            _load();
    }

    void _load() {
        _f = {};
        _f.address = reinterpret_cast<uintptr_t>(_addresses[_i]);
        if (_flags & kSymbolic) {
            mergeDlInfo(_f);
        }
    }

    Flags _flags;
    AddressMetadata _f;

    std::array<void*, kFrameMax> _addresses;
    size_t _n = 0;
    size_t _i = 0;
};

MONGO_COMPILER_NOINLINE
void printStackTrace(std::ostream& os, bool fromSignal) {
    Iteration iteration(os, fromSignal);
    printStackTraceGeneric(iteration, os, StackTraceOptions{});
}

#elif MONGO_STACKTRACE_BACKEND == MONGO_STACKTRACE_BACKEND_NONE

MONGO_COMPILER_NOINLINE
void printStackTrace(std::ostream& os, bool fromSignal) {
    os << "This platform does not support printing stacktraces" << std::endl;
}

#endif  // MONGO_STACKTRACE_BACKEND

}  // namespace stacktrace_detail

MONGO_COMPILER_NOINLINE
void printStackTrace(std::ostream& os) {
    stacktrace_detail::printStackTrace(os, false);
}

MONGO_COMPILER_NOINLINE
void printStackTraceFromSignal(std::ostream& os) {
    stacktrace_detail::printStackTrace(os, true);
}

}  // namespace mongo
