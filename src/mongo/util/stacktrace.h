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

/**
 * Tools for working with in-process stack traces.
 */
#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/util/future.h"
#include "mongo/util/synchronized_value.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <utility>

/**
 * All-thread backtrace is only implemented on Linux. Even on Linux, it's only AS-safe
 * when we are using the libunwind backtrace implementation. The feature and related
 * functions are only defined when `MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS`:
 *    - setupStackTraceSignalAction
 *    - markAsStackTraceProcessingThread
 *    - printAllThreadStacks
 */
#if defined(__linux__) && defined(MONGO_CONFIG_USE_LIBUNWIND)
#define MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS
#endif

namespace mongo {

const size_t kStackTraceFrameMax = 100;

/** Abstract sink onto which stacktrace is piecewise emitted. */
class StackTraceSink {
public:
    StackTraceSink& operator<<(StringData v) {
        doWrite(v);
        return *this;
    }

private:
    virtual void doWrite(StringData v) = 0;
};

class OstreamStackTraceSink : public StackTraceSink {
public:
    explicit OstreamStackTraceSink(std::ostream& os) : _os(os) {}

private:
    void doWrite(StringData v) override {
        _os << v;
    }
    std::ostream& _os;
};

class StringStackTraceSink : public StackTraceSink {
public:
    StringStackTraceSink(std::string& s) : _s{s} {}

private:
    void doWrite(StringData v) override {
        _s.append(v.data(), v.size());
    }

    std::string& _s;
};

/**
 * A `StackTrace` object also encapsulates any errors encountered while attaining stacktrace
 * information. Oddly, a `StackTrace` object can be in an error state (`hasError` returns true) and
 * have non-empty stacktrace information via `getBSONRepresentation`. It is legal to call
 * `getBSONRepresentation` even when in an error state.
 *
 * Likewise, it is always safe to call `log` or `sink`, regardless of error state. Those output
 * methods will write out any errors along with any available stacktrace information.
 *
 * Disabling log truncation is strongly recommended when logging a BSONObj returned from
 * `getBSONRepresentation` by hand.
 */
class StackTrace {
public:
    explicit StackTrace(BSONObj stacktrace) : _stacktrace(stacktrace) {}

    StackTrace(BSONObj stacktrace, std::string error)
        : _stacktrace(stacktrace), _error(std::move(error)) {}

    void log(bool withHumanReadable = true) const;

    void sink(StackTraceSink* sink, bool withHumanReadable = true) const;

    BSONObj getBSONRepresentation() const {
        return _stacktrace;
    }

    bool hasError() const {
        return !_error.empty();
    }

    std::string getError() const {
        return _error;
    }

private:
    BSONObj _stacktrace;
    std::string _error;
};

namespace stack_trace_detail {
/**
 * A utility for uint64_t <=> uppercase hex string conversions. It
 * can be used to produce a StringData.
 *
 *     sink << Hex(x);  // as a temporary
 *
 *     Hex hx(x);
 *     StringData sd = hx;  // sd storage is in `hx`.
 */
class Hex {
public:
    using Buf = std::array<char, 18>;  // 64/4 hex digits plus potential "0x"

    static StringData toHex(uint64_t x, Buf& buf, bool showBase = false);

    static uint64_t fromHex(StringData s);

    explicit Hex(uint64_t x, bool showBase = false) : _str{toHex(x, _buf, showBase)} {}
    explicit Hex(const void* x, bool showBase = false)
        : Hex{reinterpret_cast<uintptr_t>(x), showBase} {}

    operator StringData() const {
        return _str;
    }

private:
    Buf _buf;
    StringData _str;
};

class Dec {
public:
    using Buf = std::array<char, 20>;  // ceil(64*log10(2))

    static StringData toDec(uint64_t x, Buf& buf);

    static uint64_t fromDec(StringData s);

    explicit Dec(uint64_t x) : _str(toDec(x, _buf)) {}

    operator StringData() const {
        return _str;
    }

private:
    Buf _buf;
    StringData _str;
};

void logBacktraceObject(const BSONObj& bt, StackTraceSink* sink, bool withHumanReadable);

/**
 * Multiple waiters can register their interest in the next stack trace,
 * by calling `waiter()` and obtaining a waiter object, which will block
 * in its destructor until the next stack trace completes.
 * On the "producer side", each stack trace collection calls `notifier()`
 * to become that next stack trace. It will signal the end of the collection
 * by destroying the notifier object returned by that `notifier` call.
 */
class PrintAllStacksSession {
public:
    /** Notifies observers on its destruction. */
    class Notifier {
    public:
        explicit Notifier(std::unique_ptr<SharedPromise<void>> prom) : _prom{std::move(prom)} {}
        ~Notifier() {
            if (_prom)
                _prom->emplaceValue();
        }

        Notifier(Notifier&&) = default;
        Notifier& operator=(Notifier&&) = default;

    private:
        std::unique_ptr<SharedPromise<void>> _prom;
    };

    /** Blocks in its destructor waiting for a session to complete. */
    class Waiter {
    public:
        explicit Waiter(SharedSemiFuture<void> fut) : _fut{std::move(fut)} {}
        ~Waiter() {
            if (_fut.valid())
                _fut.get();
        }

        Waiter(Waiter&&) = default;
        Waiter& operator=(Waiter&&) = default;

    private:
        SharedSemiFuture<void> _fut;
    };

    Notifier notifier() {
        // Consume and retain the current SharedPromise from _promise.
        return Notifier{std::exchange(**_promise, {})};
    }

    Waiter waiter() {
        auto updateGuard = _promise.synchronize();
        if (!*updateGuard)
            *updateGuard = std::make_unique<SharedPromise<void>>();
        return Waiter{(*updateGuard)->getFuture()};
    }

private:
    synchronized_value<std::unique_ptr<SharedPromise<void>>> _promise;
};

}  // namespace stack_trace_detail

#ifndef _WIN32
/**
 * Metadata about an instruction address.
 * Beyond that, it may have an enclosing shared object file.
 * Further, it may have an enclosing symbol within that shared object.
 *
 * Support for StackTraceAddressMetadata is unimplemented on Windows.
 */
class StackTraceAddressMetadata {
public:
    struct BaseAndName {
        /** Disengaged when _base is null. */
        explicit operator bool() const {
            return _base != 0;
        }

        void clear() {
            _base = 0;
            _name.clear();
        }

        void assign(uintptr_t newBase, StringData newName) {
            _base = newBase;
            if (newBase != 0)
                _name.assign(newName.begin(), newName.end());
            else
                _name.clear();
        }

        uintptr_t base() const {
            return _base;
        }
        StringData name() const {
            return _name;
        }

        uintptr_t _base{};
        std::string _name;
    };

    StackTraceAddressMetadata() = default;

    uintptr_t address() const {
        return _address;
    }
    const BaseAndName& file() const {
        return _file;
    }
    const BaseAndName& symbol() const {
        return _symbol;
    }
    BaseAndName& file() {
        return _file;
    }
    BaseAndName& symbol() {
        return _symbol;
    }

    void reset(uintptr_t addr = 0) {
        _address = addr;
        _file.assign(0, {});
        _symbol.assign(0, {});
    }

    void setAddress(uintptr_t address) {
        _address = address;
    }

    void printTo(StackTraceSink& sink) const;

private:
    uintptr_t _address{};
    BaseAndName _file{};
    BaseAndName _symbol{};
};

/**
 * Retrieves metadata for program addresses.
 * Manages string storage internally as an optimization.
 *
 * Example:
 *
 *    struct CapturedEvent {
 *       std::array<void*, kStackTraceFramesMax> trace;
 *       size_t traceSize;
 *       // ...
 *    };
 *
 *    CapturedEvent* event = ...
 *    // In a performance-sensitive event handler, capture a raw trace.
 *    event->traceSize = mongo::rawBacktrace(event->trace.data(), event->trace.size());
 *
 *    // Elsewhere, print a detailed trace of the captured event to a `sink`.
 *    CapturedEvent* event = ...
 *    StackTraceAddressMetadataGenerator metaGen;
 *    void** ptr = event->trace.data();
 *    void** ptrEnd = event->trace.data() + event->traceSize;
 *    std::for_each(ptr, ptrEnd, [](void* addr) {
 *        const auto& meta = metaGen.load(addr);
 *        meta.printTo(sink);
 *    }
 */
class StackTraceAddressMetadataGenerator {
public:
    /**
     * Fill the internal meta structure with the metadata of `address`.
     * The returned reference is valid until the next call to `load`.
     */
    const StackTraceAddressMetadata& load(void* address);

    /** Access the internal metadata object without changing anything. */
    const StackTraceAddressMetadata& meta() const {
        return _meta;
    }

private:
    StackTraceAddressMetadata _meta;
};

/**
 * Loads a raw backtrace into the `void*` range `[addrs, addrs + capacity)`.
 * Returns number frames reported.
 * AS-Unsafe with gnu libc.
 *    https://www.gnu.org/software/libc/manual/html_node/Backtraces.html
 * AS-Safe with libunwind.
 */
size_t rawBacktrace(void** addrs, size_t capacity);

#endif  // _WIN32

// Print stack trace information to a sink, defaults to the log stream.
void printStackTrace(StackTraceSink& sink);
void printStackTrace(std::ostream& os);
void printStackTrace();
StackTrace getStackTrace();

#if defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)

/**
 * Called from single-thread init time. Initializes the `printAllThreadStacks` system,
 * which will be triggered by `signal`. Threads must not block this `signal`.
 */
void setupStackTraceSignalAction(int signal);

/**
 * External stack trace request signals are forwarded to the thread that calls this function.
 * That thread should call `printAllThreadStacks` when it receives the stack trace signal.
 */
void markAsStackTraceProcessingThread();

/**
 * Provides a means for a server to dump all thread stacks in response to an
 * asynchronous signal from an external `kill`. The signal processing thread calls this
 * function when it receives the signal for the process. This function then sends the
 * same signal via `tgkill` to every other thread and collects their responses.
 */
void printAllThreadStacks();
void printAllThreadStacks(StackTraceSink& sink);

/** Calls `printAllThreadStacks` and blocks until it is completed. */
void printAllThreadStacksBlocking();

#endif  // defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)

}  // namespace mongo
