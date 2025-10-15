/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/util/stacktrace.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/util/ctype.h"

#include <algorithm>
#include <iterator>
#include <sstream>

#include <fmt/format.h>

#ifdef MONGO_CONFIG_DEV_STACKTRACE
#include "mongo/logv2/log_plain.h"

#include <cpptrace/formatting.hpp>
#include <cpptrace/utils.hpp>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
namespace stack_trace_detail {
namespace {

template <size_t base>
StringData kDigits;
template <>
constexpr StringData kDigits<16> = "0123456789ABCDEF"_sd;
template <>
constexpr StringData kDigits<10> = "0123456789"_sd;

template <size_t base, typename Buf>
StringData toNumericBase(uint64_t x, Buf& buf, bool showBase) {
    auto it = buf.rbegin();
    if (!x) {
        *it++ = '0';
    } else {
        for (; x; ++it) {
            *it = kDigits<base>[x % base];
            x /= base;
        }
        // base is prepended only when x is nonzero (matching printf)
        if (base == 16 && showBase) {
            static const auto kPrefix = "0x"_sd;
            it = std::reverse_copy(kPrefix.begin(), kPrefix.end(), it);
        }
    }
    size_t n = std::distance(it.base(), buf.end());
    const char* p = buf.data() + buf.size() - n;
    return StringData(p, n);
}

}  // namespace

StringData Dec::toDec(uint64_t x, Buf& buf) {
    return toNumericBase<10>(x, buf, false);
}

StringData Hex::toHex(uint64_t x, Buf& buf, bool showBase) {
    return toNumericBase<16>(x, buf, showBase);
}

uint64_t Hex::fromHex(StringData s) {
    uint64_t x = 0;
    for (char c : s) {
        char uc = ctype::toUpper(c);
        if (size_t pos = kDigits<16>.find(uc); pos == std::string::npos) {
            return x;
        } else {
            x <<= 4;
            x += pos;
        }
    }
    return x;
}

#ifdef MONGO_CONFIG_DEV_STACKTRACE

/**
 * Determine whether the context running this program supports colorized output.
 */
bool supportsColor() {
    if (isatty(STDOUT_FILENO))
        return true;
    // TODO: support color when run under `bazel test`.
    return false;
}

void printCppTrace(StackTraceSink* sink) {
    auto trace = cpptrace::generate_trace(0, kStackTraceFrameMax);
    static StaticImmortal<cpptrace::formatter> baseFormat =
        cpptrace::formatter{}
            .break_before_filename(true)
            .symbols(cpptrace::formatter::symbol_mode::pretty)
            .transform([](cpptrace::stacktrace_frame f) {
                // Strip off bazel prefix to make filenames clickable.
                constexpr auto prefix = toStdStringViewForInterop("./"_sd);
                if (f.filename.starts_with(prefix)) {
                    f.filename.erase(0, prefix.size());
                }
                return f;
            });
    static const bool shouldColor = supportsColor();

    cpptrace::formatter formatter;
    if (sink == nullptr && shouldColor) {
        formatter = baseFormat->colors(cpptrace::formatter::color_mode::always);
    } else {
        formatter = baseFormat->colors(cpptrace::formatter::color_mode::none);
    }

    std::ostringstream btss;
    btss << "BACKTRACE:\n";
    for (size_t i = 0; i < trace.frames.size(); ++i) {
        auto&& frame = trace.frames[i];
        btss << "#" << i << " " << formatter.format(frame) << std::endl;
        if (frame.filename.find("src/mongo/") != std::string::npos && frame.line.has_value()) {
            btss << cpptrace::get_snippet(
                        frame.filename, frame.line.value(), frame.column, 2, shouldColor)
                 << "\n";
        }
    }
    if (sink) {
        *sink << btss.str();
    } else {
        logv2::plainLogBypass(btss.str());
    }
}
#endif

void logBacktraceObject(const BSONObj& bt, StackTraceSink* sink, bool withHumanReadable) {
    if (sink) {
        *sink << fmt::format("BACKTRACE: {}\n", tojson(bt, ExtendedRelaxedV2_0_0));
    } else {
        LOGV2_OPTIONS(
            31380,
            mongo::logv2::LogOptions(logv2::LogTag::kBacktraceLog, logv2::LogTruncation::Disabled),
            "BACKTRACE",
            "bt"_attr = bt);
    }
    if (withHumanReadable) {
        if (auto elem = bt.getField("backtrace"); !elem.eoo()) {
            for (const auto& fe : elem.Obj()) {
                BSONObj frame = fe.Obj();
                if (sink) {
                    *sink << fmt::format("  Frame: {}\n", tojson(frame, ExtendedRelaxedV2_0_0));
                } else {
                    LOGV2(31445, "Frame", "frame"_attr = frame);
                }
            }
        }
    }
}

}  // namespace stack_trace_detail

void StackTrace::log(bool withHumanReadable) const {
    if (hasError()) {
        LOGV2_ERROR(31430, "Error collecting stack trace", "error"_attr = _error);
    }

    sink(nullptr, withHumanReadable);
}

void StackTrace::sink(StackTraceSink* sink, bool withHumanReadable) const {
    if (hasError()) {
        *sink << fmt::format("Error collecting stack trace: {}", _error);
    }

    stack_trace_detail::logBacktraceObject(_stacktrace, sink, withHumanReadable);
}

void printStackTrace(StackTraceSink& sink) {
#ifdef MONGO_CONFIG_DEV_STACKTRACE
    stack_trace_detail::printCppTrace(&sink);
#else
    printStructuredStackTrace(sink);
#endif
}

void printStackTrace(std::ostream& os) {
#ifdef MONGO_CONFIG_DEV_STACKTRACE
    OstreamStackTraceSink sink{os};
    stack_trace_detail::printCppTrace(&sink);
#else
    printStructuredStackTrace(os);
#endif
}

void printStackTrace() {
#ifdef MONGO_CONFIG_DEV_STACKTRACE
    stack_trace_detail::printCppTrace(nullptr);
#else
    printStructuredStackTrace();
#endif
}

}  // namespace mongo
