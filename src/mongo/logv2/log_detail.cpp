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


#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include <boost/log/attributes/attribute_value.hpp>
#include <boost/log/attributes/attribute_value_impl.hpp>
#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/core/record.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <fmt/args.h>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"

#ifdef _WIN32
#include <io.h>
#endif

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_internal.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_source.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/testing_proctor.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo::logv2 {
namespace {
thread_local int loggingDepth = 0;
}  // namespace

bool loggingInProgress() {
    return loggingDepth > 0;
}

void signalSafeWriteToStderr(StringData message) {
    while (!message.empty()) {
#if defined(_WIN32)
        auto ret = _write(_fileno(stderr), message.data(), message.size());
#else
        auto ret = write(STDERR_FILENO, message.data(), message.size());
#endif
        if (ret == -1) {
            if (lastPosixError() == posixError(EINTR)) {
                continue;
            }
            return;
        }
        message = message.substr(ret);
    }
}

namespace detail {

namespace {
GetTenantIDFn& getTenantID() {
    // Ensure that we avoid undefined initialization ordering
    // when logging occurs during process init and shutdown.
    // See logv2_test.cpp
    static StaticImmortal<GetTenantIDFn> fn;
    return *fn;
}
}  // namespace

void setGetTenantIDCallback(GetTenantIDFn&& fn) {
    getTenantID() = std::move(fn);
}

struct UnstructuredValueExtractor {
    void operator()(const char* name, CustomAttributeValue const& val) {
        // Prefer string serialization over BSON if available.
        if (val.stringSerialize) {
            fmt::memory_buffer buffer;
            val.stringSerialize(buffer);
            _addString(name, fmt::to_string(buffer));
        } else if (val.toString) {
            _addString(name, val.toString());
        } else if (val.BSONAppend) {
            BSONObjBuilder builder;
            val.BSONAppend(builder, name);
            // BSONObj must outlive BSONElement. See BSONElement, BSONObj::getField().
            auto obj = builder.done();
            BSONElement element = obj.getField(name);
            _addString(name, element.toString(false));
        } else if (val.BSONSerialize) {
            BSONObjBuilder builder;
            val.BSONSerialize(builder);
            (*this)(name, builder.done());
        } else if (val.toBSONArray) {
            (*this)(name, val.toBSONArray());
        }
    }

    void operator()(const char* name, const BSONObj& val) {
        StringBuilder ss;
        val.toString(ss, false);
        _addString(name, ss.str());
    }

    void operator()(const char* name, const BSONArray& val) {
        StringBuilder ss;
        val.toString(ss, true);
        _addString(name, ss.str());
    }

    template <typename Period>
    void operator()(const char* name, const Duration<Period>& val) {
        _addString(name, val.toString());
    }

    template <typename T>
    void operator()(const char* name, const T& val) {
        _args.push_back(fmt::arg(name, std::cref(val)));
    }

    void reserve(size_t n) {
        _args.reserve(n, n);
    }

    const auto& args() const {
        return _args;
    }

private:
    void _addString(const char* name, std::string&& val) {
        (*this)(name, _storage.emplace_back(std::move(val)));
    }

    fmt::dynamic_format_arg_store<fmt::format_context> _args;
    std::deque<std::string> _storage;
};

static void checkUniqueAttrs(int32_t id, const TypeErasedAttributeStorage& attrs) {
    if (attrs.size() <= 1)
        return;
    // O(N^2), but N is small and this avoids alloc, sort, and operator<.
    auto first = attrs.begin();
    auto last = attrs.end();
    while (first != last) {
        auto it = first;
        ++first;
        if (std::find_if(first, last, [&](auto&& a) { return a.name == it->name; }) == last)
            continue;
        StringData sep;
        std::string msg;
        for (auto&& a : attrs) {
            msg.append(fmt::format(R"({}"{}")", sep, a.name));
            sep = ","_sd;
        }
        uasserted(4793301, fmt::format("LOGV2 (id={}) attribute collision: [{}]", id, msg));
    }
}

static void doSafeLog(StringData reason,
                      int32_t id,
                      LogSeverity const& severity,
                      LogOptions const& options,
                      StringData message,
                      TypeErasedAttributeStorage const& attrs) {
    std::string s;
    s += fmt::format("SafeLog: {{\n");
    s += fmt::format("    reason: {:?},\n", reason);
    s += fmt::format("    loggingDepth: {},\n", loggingDepth);
    s += fmt::format("    t: {:?},\n", Date_t::now().toString());
    s += fmt::format("    severity: {:?},\n", severity.toStringData());
    s += fmt::format("    id: {},\n", id);
    s += fmt::format("    ctx: {:?},\n", getThreadName());
    s += fmt::format("    message: {:?},\n", message);
    if (!attrs.empty()) {
        s += fmt::format("    attrs: {{\n");
        attrs.apply([&]<typename T>(StringData name, const T& val) {
            s += fmt::format("        {{\n");
            s += fmt::format("            name: {:?},\n", name);
            s += fmt::format("            type: {:?},\n", demangleName(typeid(T)));
            if constexpr (std::is_integral_v<T>) {
                s += fmt::format("            value: {},\n", val);
            } else if constexpr (std::is_convertible_v<T, StringData>) {
                s += fmt::format("            value: {:?},\n", StringData{val});
            } else {
                s += fmt::format("            value: {:?},\n", "<unsupported>");
            }
            s += fmt::format("        }},\n");
        });
        s += fmt::format("    }},\n");
    }
    s += fmt::format("}}\n");
    signalSafeWriteToStderr(s);
}

void _doLogImpl(int32_t id,
                LogSeverity const& severity,
                LogOptions const& options,
                StringData message,
                TypeErasedAttributeStorage const& attrs) {
    dassert(options.component() != LogComponent::kNumLogComponents);
    // TestingProctor isEnabled cannot be called before it has been
    // initialized. But log statements occurring earlier than that still need
    // to be checked. Log performance isn't as important at startup, so until
    // the proctor is initialized, we check everything.
    if (const auto& tp = TestingProctor::instance(); !tp.isInitialized() || tp.isEnabled()) {
        checkUniqueAttrs(id, attrs);
    }

    auto& source = options.domain().internal().source();
    auto record = source.open_record(id,
                                     severity,
                                     options.component(),
                                     options.service(),
                                     options.tags(),
                                     options.truncation(),
                                     options.uassertErrorCode());
    if (record) {
        record.attribute_values().insert(
            attributes::message(),
            boost::log::attribute_value(
                new boost::log::attributes::attribute_value_impl<StringData>(message)));

        record.attribute_values().insert(
            attributes::attributes(),
            boost::log::attribute_value(
                new boost::log::attributes::attribute_value_impl<TypeErasedAttributeStorage>(
                    attrs)));

        if (auto fn = getTenantID()) {
            auto tenant = fn();
            if (!tenant.empty()) {
                record.attribute_values().insert(
                    attributes::tenant(),
                    boost::log::attribute_value(
                        new boost::log::attributes::attribute_value_impl<std::string>(tenant)));
            }
        }

        source.push_record(std::move(record));
    }
}

void doLogImpl(int32_t id,
               LogSeverity const& severity,
               LogOptions const& options,
               StringData message,
               TypeErasedAttributeStorage const& attrs) {
    if (loggingInProgress()) {
        doSafeLog("Logging in Progress", id, severity, options, message, attrs);
        return;
    }

    loggingDepth++;
    ScopeGuard updateDepth = [] {
        loggingDepth--;
    };

    try {
        _doLogImpl(id, severity, options, message, attrs);
    } catch (const fmt::format_error& ex) {
        _doLogImpl(4638200,
                   LogSeverity::Error(),
                   LogOptions(LogComponent::kAssert),
                   "Exception during log"_sd,
                   AttributeStorage{"original_msg"_attr = message, "what"_attr = ex.what()});

        invariant(!kDebugBuild, fmt::format("Exception during log: {}", ex.what()));
    } catch (...) {
        doSafeLog("Exception while creating log record", id, severity, options, message, attrs);
        throw;
    }
}

void doUnstructuredLogImpl(LogSeverity const& severity,  // NOLINT
                           LogOptions const& options,
                           StringData message,
                           TypeErasedAttributeStorage const& attrs) {

    UnstructuredValueExtractor extractor;
    extractor.reserve(attrs.size());
    attrs.apply(extractor);
    auto formatted = fmt::vformat(toStdStringViewForInterop(message), extractor.args());

    doLogImpl(0, severity, options, formatted, TypeErasedAttributeStorage());
}

}  // namespace detail

}  // namespace mongo::logv2
