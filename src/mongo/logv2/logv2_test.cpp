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


#include <array>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <forward_list>
#include <fstream>  // IWYU pragma: keep
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>  // NOLINT
#include <type_traits>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/core/swap.hpp>
#include <boost/exception/exception.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/core/core.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/tuple/tuple.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "boost/log/detail/attachable_sstream_buf.hpp"
// IWYU pragma: no_include "boost/log/detail/locking_ptr.hpp"
#include <boost/log/keywords/file_name.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/frontend_requirements.hpp>
#include <boost/log/sinks/sink.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/sinks/unlocked_frontend.hpp>
#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "boost/multi_index/detail/bidir_node_iterator.hpp"
#include <boost/none.hpp>
#include <boost/operators.hpp>
#include <boost/optional/optional.hpp>
#include <boost/parameter/keyword.hpp>
// IWYU pragma: no_include "boost/property_tree/detail/exception_implementation.hpp"
// IWYU pragma: no_include "boost/property_tree/detail/ptree_implementation.hpp"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/bson_formatter.h"
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/composite_backend.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_capture_backend.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_debug.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_domain_internal.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_source.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/log_truncation.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/logv2/ramlog.h"
#include "mongo/logv2/ramlog_sink.h"
#include "mongo/logv2/text_formatter.h"
#include "mongo/logv2/uassert_sink.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/decimal128.h"
#include "mongo/platform/int128.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str_escape.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <boost/property_tree/ptree_fwd.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/thread/exceptions.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo::logv2 {

namespace {

using constants::kAttributesFieldName;
using constants::kComponentFieldName;
using constants::kContextFieldName;
using constants::kIdFieldName;
using constants::kMessageFieldName;
using constants::kSeverityFieldName;
using constants::kTagsFieldName;
using constants::kTenantFieldName;
using constants::kTimestampFieldName;

struct TypeWithoutBSON {
    TypeWithoutBSON() {}
    TypeWithoutBSON(double x, double y) : _x(x), _y(y) {}

    double _x{0.0};
    double _y{0.0};

    std::string toString() const {
        return fmt::format("(x: {}, y: {})", _x, _y);
    }
};

struct TypeWithOnlyStringSerialize {
    TypeWithOnlyStringSerialize() {}
    TypeWithOnlyStringSerialize(double x, double y) : _x(x), _y(y) {}

    double _x{0.0};
    double _y{0.0};

    void serialize(fmt::memory_buffer& buffer) const {
        fmt::format_to(std::back_inserter(buffer), "(x: {}, y: {})", _x, _y);
    }
};

struct TypeWithBothStringFormatters {
    TypeWithBothStringFormatters() {}

    std::string toString() const {
        return fmt::format("toString");
    }

    void serialize(fmt::memory_buffer& buffer) const {
        fmt::format_to(std::back_inserter(buffer), "serialize");
    }
};

struct TypeWithBSON : TypeWithoutBSON {
    using TypeWithoutBSON::TypeWithoutBSON;

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        builder.append("x"_sd, _x);
        builder.append("y"_sd, _y);
        return builder.obj();
    }
};

struct TypeWithBSONSerialize : TypeWithoutBSON {
    using TypeWithoutBSON::TypeWithoutBSON;

    void serialize(BSONObjBuilder* builder) const {
        builder->append("x"_sd, _x);
        builder->append("y"_sd, _y);
        builder->append("type"_sd, "serialize"_sd);
    }
};

struct TypeWithBothBSONFormatters : TypeWithBSON {
    using TypeWithBSON::TypeWithBSON;

    void serialize(BSONObjBuilder* builder) const {
        builder->append("x"_sd, _x);
        builder->append("y"_sd, _y);
        builder->append("type"_sd, "serialize"_sd);
    }
};

struct TypeWithBSONArray {
    std::string toString() const {
        return "first, second";
    }
    BSONArray toBSONArray() const {
        BSONArrayBuilder builder;
        builder.append("first"_sd);
        builder.append("second"_sd);
        return builder.arr();
    }
};

enum UnscopedEnumWithToString { UnscopedEntryWithToString };

std::string toString(UnscopedEnumWithToString val) {
    return "UnscopedEntryWithToString";
}

struct TypeWithNonMemberFormatting {};

std::string toString(const TypeWithNonMemberFormatting&) {
    return "TypeWithNonMemberFormatting";
}

BSONObj toBSON(const TypeWithNonMemberFormatting&) {
    BSONObjBuilder builder;
    builder.append("first"_sd, "TypeWithNonMemberFormatting");
    return builder.obj();
}

struct TypeWithToStringForLogging {
    friend std::string toStringForLogging(const TypeWithToStringForLogging& x) {
        return "toStringForLogging";
    }
    std::string toString() const {
        return "[overridden]";
    }
};

enum EnumWithToStringForLogging { e1, e2, e3 };

inline std::string toStringForLogging(EnumWithToStringForLogging x) {
    return "[log-friendly enum]";
}

inline std::string toString(EnumWithToStringForLogging x) {
    return "[not to be used]";
}

LogManager& mgr() {
    return LogManager::global();
}

template <typename SinkPtr>
void applyDefaultFilterToSink(SinkPtr&& sink) {
    sink->set_filter(ComponentSettingsFilter(mgr().getGlobalDomain(), mgr().getGlobalSettings()));
}

class Listener : public logv2::LogLineListener {
public:
    explicit Listener(synchronized_value<std::vector<std::string>>* sv) : _sv(sv) {}
    void accept(const std::string& line) override {
        (***_sv).push_back(line);
    }

private:
    synchronized_value<std::vector<std::string>>* _sv;
};

class LogDuringInitShutdownTester {
public:
    LogDuringInitShutdownTester() {
        auto sink = LogCaptureBackend::create(std::make_unique<Listener>(&syncedLines), true);
        applyDefaultFilterToSink(sink);
        // We have to leave this sink installed as it is not allowed to install sinks during
        // shutdown. Add a filter so it is only used during this test.
        sink->set_filter([this](boost::log::attribute_value_set const& attrs) { return enabled; });
        sink->set_formatter(PlainFormatter());
        boost::log::core::get()->add_sink(sink);

        ScopeGuard enabledGuard([this] { enabled = false; });
        LOGV2(20001, "log during init");
        ASSERT_EQUALS((**syncedLines).back(), "log during init");
    }
    ~LogDuringInitShutdownTester() {
        enabled = true;
        LOGV2(4600800, "log during shutdown");
        ASSERT_EQUALS((**syncedLines).back(), "log during shutdown");
    }

    synchronized_value<std::vector<std::string>> syncedLines;
    bool enabled = true;
};

LogDuringInitShutdownTester logDuringInitAndShutdown;

class LogV2Test : public unittest::Test {
public:
    class LineCapture {
    public:
        LineCapture() = delete;
        LineCapture(bool stripEol)
            : _syncedLines{synchronized_value<std::vector<std::string>>()},
              _sink{
                  LogCaptureBackend::create(std::make_unique<Listener>(&_syncedLines), stripEol)} {}
        auto lines() {
            return **_syncedLines;
        }
        auto& sink() {
            return _sink;
        }
        std::string back() const {
            auto logLinesLockGuard = *_syncedLines;
            ASSERT_GT(logLinesLockGuard->size(), 0);
            return logLinesLockGuard->back();
        }
        void clear() {
            return (**_syncedLines).clear();
        }
        size_t size() const {
            return (**_syncedLines).size();
        }

    private:
        synchronized_value<std::vector<std::string>> _syncedLines;
        boost::shared_ptr<boost::log::sinks::unlocked_sink<LogCaptureBackend>> _sink;
    };

    LogV2Test() {
        LogDomainGlobal::ConfigurationOptions config;
        config.makeDisabled();
        ASSERT_OK(mgr().getGlobalDomainInternal().configure(config));
    }

    ~LogV2Test() override {
        for (auto&& sink : _attachedSinks)
            boost::log::core::get()->remove_sink(sink);
        ASSERT_OK(mgr().getGlobalDomainInternal().configure({}));
    }

    template <typename T>
    static auto wrapInSynchronousSink(boost::shared_ptr<T> sink) {
        return boost::make_shared<boost::log::sinks::synchronous_sink<T>>(std::move(sink));
    }

    template <typename T>
    static auto wrapInUnlockedSink(boost::shared_ptr<T> sink) {
        return boost::make_shared<boost::log::sinks::unlocked_sink<T>>(std::move(sink));
    }

    /**
     * Take some `boost::shared_ptr<T>...` backends, and make a
     * `boost::shared_ptr<CompositeBackend<T...>>` from them.
     */
    template <typename... Ptrs>
    auto wrapInCompositeBackend(Ptrs&&... backends) {
        using B = CompositeBackend<typename Ptrs::element_type...>;
        return boost::make_shared<B>(std::forward<Ptrs>(backends)...);
    }

    void attachSink(boost::shared_ptr<boost::log::sinks::sink> sink) {
        boost::log::core::get()->add_sink(sink);
        _attachedSinks.push_back(sink);
    }

    void popSink() {
        auto sink = _attachedSinks.back();
        boost::log::core::get()->remove_sink(sink);
        _attachedSinks.pop_back();
    }

    template <typename Fmt>
    std::unique_ptr<LineCapture> makeLineCapture(Fmt&& formatter, bool stripEol = true) {
        auto ret = std::make_unique<LineCapture>(stripEol);
        auto& s = ret->sink();
        applyDefaultFilterToSink(s);
        s->set_formatter(std::forward<Fmt>(formatter));
        attachSink(s);
        return ret;
    }

private:
    std::vector<boost::shared_ptr<boost::log::sinks::sink>> _attachedSinks;
};

TEST_F(LogV2Test, Basic) {
    auto lines = makeLineCapture(PlainFormatter());

    BSONObjBuilder builder;
    fmt::memory_buffer buffer;

    LOGV2(20002, "test");
    ASSERT_EQUALS(lines->back(), "test");

    LOGV2_DEBUG(20063, -2, "test debug");
    ASSERT_EQUALS(lines->back(), "test debug");

    LOGV2(20003, "test {name}", "name"_attr = 1);
    ASSERT_EQUALS(lines->back(), "test 1");

    LOGV2(20004, "test {name:d}", "name"_attr = 2);
    ASSERT_EQUALS(lines->back(), "test 2");

    LOGV2(20005, "test {name}", "name"_attr = "char*");
    ASSERT_EQUALS(lines->back(), "test char*");

    LOGV2(20006, "test {name}", "name"_attr = std::string("std::string"));
    ASSERT_EQUALS(lines->back(), "test std::string");

    LOGV2(20007, "test {name}", "name"_attr = "StringData"_sd);
    ASSERT_EQUALS(lines->back(), "test StringData");

    LOGV2_OPTIONS(20064, {LogTag::kStartupWarnings}, "test");
    ASSERT_EQUALS(lines->back(), "test");

    TypeWithBSON t(1.0, 2.0);
    LOGV2(20008, "{name} custom formatting", "name"_attr = t);
    ASSERT_EQUALS(lines->back(), t.toString() + " custom formatting");

    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2(20009, "{name} custom formatting, no bson", "name"_attr = t2);
    ASSERT_EQUALS(lines->back(), t.toString() + " custom formatting, no bson");

    TypeWithOnlyStringSerialize t3(1.0, 2.0);
    LOGV2(20010, "{name}", "name"_attr = t3);
    buffer.clear();
    t3.serialize(buffer);
    ASSERT_EQUALS(lines->back(), fmt::to_string(buffer));

    // Serialize should be preferred when both are available
    TypeWithBothStringFormatters t4;
    LOGV2(20011, "{name}", "name"_attr = t4);
    buffer.clear();
    t4.serialize(buffer);
    ASSERT_EQUALS(lines->back(), fmt::to_string(buffer));
}

TEST_F(LogV2Test, MismatchAttrInLogging) {
    auto lines = makeLineCapture(PlainFormatter());
    if (!kDebugBuild) {
        LOGV2(4638203, "mismatch {name}", "not_name"_attr = 1);
        ASSERT(StringData(lines->back()).starts_with("Exception during log"_sd));
    }
}

TEST_F(LogV2Test, MissingAttrInLogging) {
    auto lines = makeLineCapture(PlainFormatter());
    if (!kDebugBuild) {
        LOGV2(6636803, "Log missing {attr}");
        ASSERT(StringData(lines->back()).starts_with("Exception during log"_sd));
    }
}

namespace bl_sinks = boost::log::sinks;
// Sink backend which will grab a mutex, then immediately segfault.
class ConsumeSegfaultsBackend
    : public bl_sinks::basic_formatted_sink_backend<char, bl_sinks::synchronized_feeding> {
public:
    static auto create() {
        return boost::make_shared<bl_sinks::synchronous_sink<ConsumeSegfaultsBackend>>(
            boost::make_shared<ConsumeSegfaultsBackend>());
    }

    void consume(boost::log::record_view const& rec, string_type const& formattedString) {
        if (firstRun) {
            firstRun = false;
            raise(SIGSEGV);
        } else {
            // Reentrance of consume(), which could cause deadlock. Exit normally, causing the death
            // test to fail.
            exit(static_cast<int>(ExitCode::clean));
        }
    }

private:
    bool firstRun = true;
};

// Test that signals thrown during logging will not hang process death. Uses the
// ConsumeSegfaultsBackend so that upon the initial log call, ConsumeSegfaultsBackend::consume will
// be called, sending SIGSEGV. If the signal handler incorrectly invokes the logging subsystem, the
// ConsumeSegfaultsBackend::consume function will be again invoked, failing the test since this
// could result in deadlock.
DEATH_TEST_F(LogV2Test, SIGSEGVDoesNotHang, "Got signal: ") {
    auto sink = ConsumeSegfaultsBackend::create();
    attachSink(sink);
    LOGV2(6384304, "will SIGSEGV {str}", "str"_attr = "sigsegv");
    // If we get here, we didn't segfault, and the test will fail.
}

class ConsumeThrowsBackend
    : public bl_sinks::basic_formatted_sink_backend<char, bl_sinks::synchronized_feeding> {
public:
    struct LocalException : std::exception {};
    static auto create() {
        return boost::make_shared<bl_sinks::synchronous_sink<ConsumeThrowsBackend>>(
            boost::make_shared<ConsumeThrowsBackend>());
    }

    void consume(boost::log::record_view const& rec, string_type const& formattedString) {
        throw LocalException();
    }
};

TEST_F(LogV2Test, ExceptInLogging) {
    auto sink = ConsumeThrowsBackend::create();
    attachSink(sink);
    ASSERT_THROWS(LOGV2(6636801, "will throw exception"), ConsumeThrowsBackend::LocalException);
    popSink();
}

class LogV2TypesTest : public LogV2Test {
public:
    using LogV2Test::LogV2Test;
    LogV2TypesTest() : LogV2Test() {
        detail::setGetTenantIDCallback([this]() -> std::string { return this->tenant.toString(); });
    }
    ~LogV2TypesTest() override {
        detail::setGetTenantIDCallback(nullptr);
    }

    // The JSON formatter should make the types round-trippable without data loss
    template <typename T>
    void validateJSON(T expected) {
        namespace pt = boost::property_tree;
        std::istringstream json_stream(json->back());
        pt::ptree ptree;
        pt::json_parser::read_json(json_stream, ptree);
        ASSERT_EQUALS(ptree.get<std::string>(std::string(kTenantFieldName)), tenant.toString());
        ASSERT_EQUALS(ptree.get<T>(std::string(kAttributesFieldName) + ".name"), expected);
    }

    auto lastBSONElement() {
        auto str = bson->back();
        buf.realloc(str.size());
        str.copy(buf.get(), str.size());
        BSONObj obj(buf);

        ASSERT_EQUALS(obj.getField(kTenantFieldName).String(), tenant.toString());
        container = obj.getField(kAttributesFieldName).Obj();
        return container.getField("name"_sd);
    }

    TenantId tenant = TenantId(OID::gen());
    std::unique_ptr<LineCapture> text = makeLineCapture(PlainFormatter());
    std::unique_ptr<LineCapture> json = makeLineCapture(JSONFormatter());
    std::unique_ptr<LineCapture> bson = makeLineCapture(BSONFormatter());
    SharedBuffer buf;
    // Using BSONElements returned by lastBSONElement() is safe, as they reference data stored in
    // `buf`. However, this cannot normally be enforced, so getField() is annotated
    // [[lifetimebound]]. As such, having the nested BSONObj go out of scope before the
    // BSONElement will lead to a compile time warning "-Wreturn-stack-address". Store the
    // nested object here to avoid.
    BSONObj container;
};

TEST_F(LogV2TypesTest, Numeric) {
    auto testIntegral = [&](auto dummy) {
        using T = decltype(dummy);

        auto test = [&](auto value) {
            text->clear();
            LOGV2(20012, "{name}", "name"_attr = value);
            ASSERT_EQUALS(text->back(), fmt::format("{}", value));
            validateJSON(value);

            // TODO: We should have been able to use std::make_signed here but it is broken on
            // Visual Studio 2017 and 2019
            using T = decltype(value);
            if constexpr (std::is_same_v<T, unsigned long long>) {
                ASSERT_EQUALS(lastBSONElement().Number(), static_cast<long long>(value));
            } else if constexpr (std::is_same_v<T, unsigned long>) {
                ASSERT_EQUALS(lastBSONElement().Number(), static_cast<int64_t>(value));
            } else {
                ASSERT_EQUALS(lastBSONElement().Number(), value);
            }
        };

        test(std::numeric_limits<T>::max());
        test(std::numeric_limits<T>::min());
        test(std::numeric_limits<T>::lowest());
        test(static_cast<T>(-10));
        test(static_cast<T>(-2));
        test(static_cast<T>(-1));
        test(static_cast<T>(0));
        test(static_cast<T>(1));
        test(static_cast<T>(2));
        test(static_cast<T>(10));
    };

    auto testFloatingPoint = [&](auto dummy) {
        using T = decltype(dummy);

        auto test = [&](auto value) {
            text->clear();
            LOGV2(20013, "{name}", "name"_attr = value);
            // Floats are formatted as double
            ASSERT_EQUALS(text->back(), fmt::format("{}", static_cast<double>(value)));
            validateJSON(value);
            ASSERT_EQUALS(lastBSONElement().Number(), value);
        };

        test(std::numeric_limits<T>::max());
        test(std::numeric_limits<T>::min());
        test(std::numeric_limits<T>::lowest());
        test(static_cast<T>(-10));
        test(static_cast<T>(-2));
        test(static_cast<T>(-1));
        test(static_cast<T>(0));
        test(static_cast<T>(1));
        test(static_cast<T>(2));
        test(static_cast<T>(10));
    };

    bool b = true;
    LOGV2(20014, "bool {name}", "name"_attr = b);
    ASSERT_EQUALS(text->back(), "bool true");
    validateJSON(b);
    ASSERT(lastBSONElement().Bool() == b);

    char c = 1;
    LOGV2(20015, "char {name}", "name"_attr = c);
    ASSERT_EQUALS(text->back(), "char 1");
    validateJSON(static_cast<uint8_t>(c));  // cast to prevent property_tree ASCII parse.
    ASSERT(lastBSONElement().Number() == c);

    testIntegral(static_cast<signed char>(0));
    testIntegral(static_cast<unsigned char>(0));
    testIntegral(static_cast<short>(0));
    testIntegral(static_cast<unsigned short>(0));
    testIntegral(0);
    testIntegral(0u);
    testIntegral(0l);
    testIntegral(0ul);
    testIntegral(0ll);
    testIntegral(0ull);
    testIntegral(static_cast<int64_t>(0));
    testIntegral(static_cast<uint64_t>(0));
    testIntegral(static_cast<size_t>(0));
    testFloatingPoint(0.0f);
    testFloatingPoint(0.0);
    // long double is prohibited, we don't use this type and favors Decimal128 instead.
}

// int128 is not a numeric type for the purposes of bson->
TEST_F(LogV2TypesTest, Int128) {
    auto test = [&](auto value) {
        text->clear();
        LOGV2(7497400, "uint128/int128 {name}", "name"_attr = value);
        ASSERT_EQUALS(text->back(), "uint128/int128 " + toString(value));
    };

    test(uint128_t(0));
    test(std::numeric_limits<uint128_t>::min());
    test(std::numeric_limits<uint128_t>::max());
    test(uint128_t(-10));
    test(uint128_t(-2));
    test(uint128_t(-1));
    test(uint128_t(0));
    test(uint128_t(1));
    test(uint128_t(2));
    test(uint128_t(10));

    test(int128_t(0));
    test(std::numeric_limits<int128_t>::min());
    test(std::numeric_limits<int128_t>::max());
    test(int128_t(-10));
    test(int128_t(-2));
    test(int128_t(-1));
    test(int128_t(0));
    test(int128_t(1));
    test(int128_t(2));
    test(int128_t(10));
}

TEST_F(LogV2TypesTest, Enums) {
    // enums
    enum UnscopedEnum { UnscopedEntry };
    LOGV2(20076, "{name}", "name"_attr = UnscopedEntry);
    auto expectedUnscoped = static_cast<std::underlying_type_t<UnscopedEnum>>(UnscopedEntry);
    ASSERT_EQUALS(text->back(), std::to_string(expectedUnscoped));
    validateJSON(expectedUnscoped);
    ASSERT_EQUALS(lastBSONElement().Number(), expectedUnscoped);

    enum class ScopedEnum { Entry = -1 };
    LOGV2(20077, "{name}", "name"_attr = ScopedEnum::Entry);
    auto expectedScoped = static_cast<std::underlying_type_t<ScopedEnum>>(ScopedEnum::Entry);
    ASSERT_EQUALS(text->back(), std::to_string(expectedScoped));
    validateJSON(expectedScoped);
    ASSERT_EQUALS(lastBSONElement().Number(), expectedScoped);

    LOGV2(20078, "{name}", "name"_attr = UnscopedEntryWithToString);
    ASSERT_EQUALS(text->back(), toString(UnscopedEntryWithToString));
    validateJSON(toString(UnscopedEntryWithToString));
    ASSERT_EQUALS(lastBSONElement().String(), toString(UnscopedEntryWithToString));
}

TEST_F(LogV2TypesTest, Stringlike) {
    const char* c_str = "a c string";
    LOGV2(20016, "c string {name}", "name"_attr = c_str);
    ASSERT_EQUALS(text->back(), "c string a c string");
    validateJSON(std::string(c_str));
    ASSERT_EQUALS(lastBSONElement().String(), c_str);

    char* c_str2 = const_cast<char*>("non-const");
    LOGV2(20017, "c string {name}", "name"_attr = c_str2);
    ASSERT_EQUALS(text->back(), "c string non-const");
    validateJSON(std::string(c_str2));
    ASSERT_EQUALS(lastBSONElement().String(), c_str2);

    std::string str = "a std::string";
    LOGV2(20018, "std::string {name}", "name"_attr = str);
    ASSERT_EQUALS(text->back(), "std::string a std::string");
    validateJSON(str);
    ASSERT_EQUALS(lastBSONElement().String(), str);

    StringData str_data = "a StringData"_sd;
    LOGV2(20019, "StringData {name}", "name"_attr = str_data);
    ASSERT_EQUALS(text->back(), "StringData a StringData");
    validateJSON(std::string{str_data});
    ASSERT_EQUALS(lastBSONElement().String(), str_data);

    {
        std::string_view s = "a std::string_view";  // NOLINT
        LOGV2(4329200, "std::string_view {name}", "name"_attr = s);
        ASSERT_EQUALS(text->back(), "std::string_view a std::string_view");
        validateJSON(std::string{s});
        ASSERT_EQUALS(lastBSONElement().String(), s);
    }
}

TEST_F(LogV2TypesTest, BSONObj) {
    BSONObj bsonObj = BSONObjBuilder()
                          .append("int32"_sd, 1)
                          .append("int64"_sd, std::numeric_limits<int64_t>::max())
                          .append("double"_sd, 1.0)
                          .append("str"_sd, "a StringData"_sd)
                          .obj();
    LOGV2(20020, "bson {name}", "name"_attr = bsonObj);
    ASSERT(text->back() ==
           std::string("bson ") + bsonObj.jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0));
    ASSERT(mongo::fromjson(json->back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Obj()
               .woCompare(bsonObj) == 0);
    ASSERT(lastBSONElement().Obj().woCompare(bsonObj) == 0);
}

TEST_F(LogV2TypesTest, BSONArray) {
    BSONArray bsonArr =
        BSONArrayBuilder().append("first"_sd).append("second"_sd).append("third"_sd).arr();
    LOGV2(20021, "{name}", "name"_attr = bsonArr);
    ASSERT_EQUALS(text->back(),
                  bsonArr.jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true));
    ASSERT(mongo::fromjson(json->back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Obj()
               .woCompare(bsonArr) == 0);
    ASSERT(lastBSONElement().Obj().woCompare(bsonArr) == 0);
}

TEST_F(LogV2TypesTest, BSONElement) {
    BSONObj bsonObj = BSONObjBuilder()
                          .append("int32"_sd, 1)
                          .append("int64"_sd, std::numeric_limits<int64_t>::max())
                          .append("double"_sd, 1.0)
                          .append("str"_sd, "a StringData"_sd)
                          .obj();
    LOGV2(20022, "bson element {name}", "name"_attr = bsonObj.getField("int32"_sd));
    ASSERT(text->back() == std::string("bson element ") + bsonObj.getField("int32"_sd).toString());
    ASSERT(mongo::fromjson(json->back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name"_sd)
               .Obj()
               .getField("int32"_sd)
               .Int() == bsonObj.getField("int32"_sd).Int());
    ASSERT(lastBSONElement().Obj().getField("int32"_sd).Int() ==
           bsonObj.getField("int32"_sd).Int());
}

TEST_F(LogV2TypesTest, DateT) {
    bool prevIsLocalTimezone = dateFormatIsLocalTimezone();
    for (auto localTimezone : {true, false}) {
        setDateFormatIsLocalTimezone(localTimezone);
        Date_t date = Date_t::now();
        LOGV2(20023, "Date_t {name}", "name"_attr = date);
        ASSERT_EQUALS(text->back(), std::string("Date_t ") + date.toString());
        ASSERT_EQUALS(mongo::fromjson(json->back())
                          .getField(kAttributesFieldName)
                          .Obj()
                          .getField("name")
                          .Date(),
                      date);
        ASSERT_EQUALS(lastBSONElement().Date(), date);
    }

    setDateFormatIsLocalTimezone(prevIsLocalTimezone);
}

TEST_F(LogV2TypesTest, Decimal128) {
    LOGV2(20024, "Decimal128 {name}", "name"_attr = Decimal128::kPi);
    ASSERT_EQUALS(text->back(), std::string("Decimal128 ") + Decimal128::kPi.toString());
    ASSERT(mongo::fromjson(json->back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Decimal()
               .isEqual(Decimal128::kPi));
    ASSERT(lastBSONElement().Decimal().isEqual(Decimal128::kPi));
}

TEST_F(LogV2TypesTest, OID) {
    OID oid = OID::gen();
    LOGV2(20025, "OID {name}", "name"_attr = oid);
    ASSERT_EQUALS(text->back(), std::string("OID ") + oid.toString());
    ASSERT_EQUALS(
        mongo::fromjson(json->back()).getField(kAttributesFieldName).Obj().getField("name").OID(),
        oid);
    ASSERT_EQUALS(lastBSONElement().OID(), oid);
}

TEST_F(LogV2TypesTest, Timestamp) {
    Timestamp ts = Timestamp::max();
    LOGV2(20026, "Timestamp {name}", "name"_attr = ts);
    ASSERT_EQUALS(text->back(), std::string("Timestamp ") + ts.toString());
    ASSERT_EQUALS(mongo::fromjson(json->back())
                      .getField(kAttributesFieldName)
                      .Obj()
                      .getField("name")
                      .timestamp(),
                  ts);
    ASSERT_EQUALS(lastBSONElement().timestamp(), ts);
}

TEST_F(LogV2TypesTest, UUID) {
    UUID uuid = UUID::gen();
    LOGV2(20027, "UUID {name}", "name"_attr = uuid);
    ASSERT_EQUALS(text->back(), std::string("UUID ") + uuid.toString());
    ASSERT_EQUALS(UUID::parse(mongo::fromjson(json->back())
                                  .getField(kAttributesFieldName)
                                  .Obj()
                                  .getField("name")
                                  .Obj()),
                  uuid);
    ASSERT_EQUALS(UUID::parse(lastBSONElement().Obj()), uuid);
}

TEST_F(LogV2TypesTest, BoostOptional) {
    LOGV2(20028, "boost::optional empty {name}", "name"_attr = boost::optional<bool>());
    ASSERT_EQUALS(text->back(),
                  std::string("boost::optional empty ") + constants::kNullOptionalString);
    ASSERT(mongo::fromjson(json->back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .isNull());
    ASSERT(lastBSONElement().isNull());

    LOGV2(20029, "boost::optional<bool> {name}", "name"_attr = boost::optional<bool>(true));
    ASSERT_EQUALS(text->back(), std::string("boost::optional<bool> true"));
    ASSERT_EQUALS(
        mongo::fromjson(json->back()).getField(kAttributesFieldName).Obj().getField("name").Bool(),
        true);
    ASSERT_EQUALS(lastBSONElement().Bool(), true);

    LOGV2(20030,
          "boost::optional<boost::optional<bool>> {name}",
          "name"_attr = boost::optional<boost::optional<bool>>(boost::optional<bool>(true)));
    ASSERT_EQUALS(text->back(), std::string("boost::optional<boost::optional<bool>> true"));
    ASSERT_EQUALS(
        mongo::fromjson(json->back()).getField(kAttributesFieldName).Obj().getField("name").Bool(),
        true);
    ASSERT_EQUALS(lastBSONElement().Bool(), true);

    TypeWithBSON withBSON(1.0, 2.0);
    LOGV2(20031,
          "boost::optional<TypeWithBSON> {name}",
          "name"_attr = boost::optional<TypeWithBSON>(withBSON));
    ASSERT_EQUALS(text->back(),
                  std::string("boost::optional<TypeWithBSON> ") + withBSON.toString());
    ASSERT(mongo::fromjson(json->back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Obj()
               .woCompare(withBSON.toBSON()) == 0);
    ASSERT(lastBSONElement().Obj().woCompare(withBSON.toBSON()) == 0);

    TypeWithoutBSON withoutBSON(1.0, 2.0);
    LOGV2(20032,
          "boost::optional<TypeWithBSON> {name}",
          "name"_attr = boost::optional<TypeWithoutBSON>(withoutBSON));
    ASSERT_EQUALS(text->back(),
                  std::string("boost::optional<TypeWithBSON> ") + withoutBSON.toString());
    ASSERT_EQUALS(mongo::fromjson(json->back())
                      .getField(kAttributesFieldName)
                      .Obj()
                      .getField("name")
                      .String(),
                  withoutBSON.toString());
    ASSERT_EQUALS(lastBSONElement().String(), withoutBSON.toString());
}

TEST_F(LogV2TypesTest, Duration) {
    Milliseconds ms{12345};
    LOGV2(20033, "Duration {name}", "name"_attr = ms);
    ASSERT_EQUALS(text->back(), std::string("Duration ") + ms.toString());
    ASSERT_EQUALS(mongo::fromjson(json->back())
                      .getField(kAttributesFieldName)
                      .Obj()
                      .getField("name" + ms.mongoUnitSuffix())
                      .Int(),
                  ms.count());
    ASSERT_EQUALS(BSONObj(bson->back().data())
                      .getField(kAttributesFieldName)
                      .Obj()
                      .getField("name" + ms.mongoUnitSuffix())
                      .Long(),
                  ms.count());
}

void exceptionThrower() {
    uasserted(7733401, "exception in logger");
}

template <typename T>
void testExceptionHandling(T arg,
                           std::unique_ptr<LogV2Test::LineCapture> text,
                           std::unique_ptr<LogV2Test::LineCapture> json) {
    LOGV2(7733402, "test1 {a1}", "a1"_attr = arg);

    ASSERT_EQ(
        mongo::fromjson(json->back()).getField(kAttributesFieldName).Obj().getField("a1").String(),
        "Failed to serialize due to exception: Location7733401: exception in logger");

    ASSERT_EQ(text->back(),
              "test1 Failed to serialize due to exception: Location7733401: exception in logger");
}

// Throw an exception in a BSON serialization method
TEST_F(LogV2TypesTest, AttrExceptionBSONSerialize) {
    struct TypeWithBSONSerialize {
        void serialize(BSONObjBuilder*) const {
            exceptionThrower();
        }
    };

    testExceptionHandling(TypeWithBSONSerialize(), std::move(text), std::move(json));
}

// Throw an exception in a BSON Array serialization method
TEST_F(LogV2TypesTest, AttrExceptionBSONToARray) {
    struct TypeToArray {
        BSONArray toBSONArray() const {
            exceptionThrower();
            return {};
        }
    };

    testExceptionHandling(TypeToArray(), std::move(text), std::move(json));
}


TEST_F(LogV2Test, TextFormat) {
    auto lines = makeLineCapture(TextFormatter());

    LOGV2_OPTIONS(20065, {LogTag::kNone}, "warning");
    ASSERT(lines->back().rfind("** WARNING: warning") == std::string::npos);

    LOGV2_OPTIONS(20066, {LogTag::kStartupWarnings}, "warning");
    ASSERT(lines->back().rfind("** WARNING: warning") != std::string::npos);

    LOGV2_OPTIONS(20067,
                  {static_cast<LogTag::Value>(LogTag::kStartupWarnings | LogTag::kPlainShell)},
                  "warning");
    ASSERT(lines->back().rfind("** WARNING: warning") != std::string::npos);

    TypeWithBSON t(1.0, 2.0);
    LOGV2(20034, "{name} custom formatting", "name"_attr = t);
    ASSERT(lines->back().rfind(t.toString() + " custom formatting") != std::string::npos);

    LOGV2(20035, "{name} bson", "name"_attr = t.toBSON());
    ASSERT(lines->back().rfind(t.toBSON().jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0) +
                               " bson") != std::string::npos);

    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2(20036, "{name} custom formatting, no bson", "name"_attr = t2);
    ASSERT(lines->back().rfind(t.toString() + " custom formatting, no bson") != std::string::npos);

    TypeWithNonMemberFormatting t3;
    LOGV2(20079, "{name}", "name"_attr = t3);
    ASSERT(lines->back().rfind(toString(t3)) != std::string::npos);
}

TEST_F(LogV2Test, StructToStringForLogging) {
    auto lines = makeLineCapture(JSONFormatter());
    TypeWithToStringForLogging x;
    LOGV2(7496800, "Msg", "x"_attr = x);
    ASSERT_STRING_CONTAINS(lines->back(), toStringForLogging(x));
}

TEST_F(LogV2Test, EnumToStringForLogging) {
    auto lines = makeLineCapture(JSONFormatter());
    auto e = EnumWithToStringForLogging::e1;
    LOGV2(7496801, "Msg", "e"_attr = e);
    ASSERT_STRING_CONTAINS(lines->back(), toStringForLogging(e));
}

std::string hello() {
    return "hello";
}

// Runs the same validator on the json and bson versions of the logs to ensure consistency
// between them.
class LogV2JsonBsonTest : public LogV2Test {
public:
    using LogV2Test::LogV2Test;

    template <typename F>
    void validate(F validator) {
        validator(mongo::fromjson(lines->back()));
        validator(BSONObj(linesBson->back().data()));
    }

    std::unique_ptr<LineCapture> lines = makeLineCapture(JSONFormatter());
    std::unique_ptr<LineCapture> linesBson = makeLineCapture(BSONFormatter());
};

TEST_F(LogV2JsonBsonTest, Root) {
    LOGV2(20037, "test");
    validate([](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kTimestampFieldName).Date(), Date_t::lastNowForTest());
        ASSERT_EQUALS(obj.getField(kSeverityFieldName).String(),
                      LogSeverity::Info().toStringDataCompact());
        ASSERT_EQUALS(obj.getField(kComponentFieldName).String(),
                      LogComponent(MONGO_LOGV2_DEFAULT_COMPONENT).getNameForLog());
        ASSERT_EQUALS(obj.getField(kContextFieldName).String(), getThreadName());
        ASSERT_EQUALS(obj.getField(kIdFieldName).Int(), 20037);
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test");
        ASSERT(!obj.hasField(kAttributesFieldName));
        ASSERT(!obj.hasField(kTagsFieldName));
    });
}

TEST_F(LogV2JsonBsonTest, Attr) {
    LOGV2(20038, "test {name}", "name"_attr = 1);
    validate([](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test {name}");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").Int(), 1);
    });
}

TEST_F(LogV2JsonBsonTest, MessageReconstructionBasic) {
    LOGV2(20039, "test {name:d}", "name"_attr = 2);
    validate([](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test {name:d}");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").Int(), 2);
    });
}

TEST_F(LogV2JsonBsonTest, MessageReconstructionPadded) {
    LOGV2(20040, "test {name: <4}", "name"_attr = 2);
    validate([](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test {name: <4}");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").Int(), 2);
    });
}

TEST_F(LogV2JsonBsonTest, Tags) {
    LOGV2_OPTIONS(20068, {LogTag::kStartupWarnings}, "warning");
    validate([](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "warning");
        ASSERT_EQUALS(
            obj.getField("tags"_sd).Obj().woCompare(LogTag(LogTag::kStartupWarnings).toBSONArray()),
            0);
    });
}

TEST_F(LogV2JsonBsonTest, Component) {
    LOGV2_OPTIONS(20069, {LogComponent::kControl}, "different component");
    validate([](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField("c"_sd).String(),
                      LogComponent(LogComponent::kControl).getNameForLog());
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "different component");
    });
}

TEST_F(LogV2JsonBsonTest, CustomFormatting) {
    TypeWithBSON t(1.0, 2.0);
    LOGV2(20041, "{name} custom formatting", "name"_attr = t);
    validate([&t](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "{name} custom formatting");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT(
            obj.getField(kAttributesFieldName).Obj().getField("name").Obj().woCompare(t.toBSON()) ==
            0);
    });
}

TEST_F(LogV2JsonBsonTest, BsonAttr) {
    TypeWithBSON t(1.0, 2.0);
    LOGV2(20042, "{name} bson", "name"_attr = t.toBSON());
    validate([&t](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "{name} bson");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT(
            obj.getField(kAttributesFieldName).Obj().getField("name").Obj().woCompare(t.toBSON()) ==
            0);
    });
}

TEST_F(LogV2JsonBsonTest, TypeWithoutBSON) {
    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2(20043, "{name} custom formatting", "name"_attr = t2);
    validate([&t2](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "{name} custom formatting");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").String(),
                      t2.toString());
    });
}

TEST_F(LogV2JsonBsonTest, TypeWithBSONSerialize) {
    TypeWithBSONSerialize t3(1.0, 2.0);
    LOGV2(20044, "{name}", "name"_attr = t3);
    validate([&t3](const BSONObj& obj) {
        BSONObjBuilder builder;
        t3.serialize(&builder);
        ASSERT(obj.getField(kAttributesFieldName)
                   .Obj()
                   .getField("name")
                   .Obj()
                   .woCompare(builder.done()) == 0);
    });
}

TEST_F(LogV2JsonBsonTest, TypeWithoutBSONFormatters) {
    TypeWithBothBSONFormatters t4(1.0, 2.0);
    LOGV2(20045, "{name}", "name"_attr = t4);
    validate([&t4](const BSONObj& obj) {
        BSONObjBuilder builder;
        t4.serialize(&builder);
        ASSERT(obj.getField(kAttributesFieldName)
                   .Obj()
                   .getField("name")
                   .Obj()
                   .woCompare(builder.done()) == 0);
    });
}

TEST_F(LogV2JsonBsonTest, TypeWithBSONArray) {
    TypeWithBSONArray t5;
    LOGV2(20046, "{name}", "name"_attr = t5);
    validate([&t5](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").type(),
                      BSONType::array);
        ASSERT(obj.getField(kAttributesFieldName)
                   .Obj()
                   .getField("name")
                   .Obj()
                   .woCompare(t5.toBSONArray()) == 0);
    });
}

TEST_F(LogV2JsonBsonTest, TypeWithNonMemberFormatting) {
    TypeWithNonMemberFormatting t6;
    LOGV2(20080, "{name}", "name"_attr = t6);
    validate([&t6](const BSONObj& obj) {
        ASSERT_EQUALS(
            obj.getField(kAttributesFieldName).Obj().getField("name").Obj().woCompare(toBSON(t6)),
            0);
    });
}

TEST_F(LogV2JsonBsonTest, DynamicAttributes) {
    DynamicAttributes attrs;
    attrs.add("string data", "a string data"_sd);
    attrs.add("cstr", "a c string");
    attrs.add("int", 5);
    attrs.add("float", 3.0f);
    attrs.add("bool", true);
    attrs.add("enum", UnscopedEntryWithToString);
    TypeWithNonMemberFormatting t6;
    attrs.add("custom", t6);
    attrs.addUnsafe("unsafe but ok", 1);
    BSONObj bsonObj;
    attrs.add("bson", bsonObj);
    attrs.add("millis", Milliseconds(1));
    attrs.addDeepCopy("stdstr", hello());
    LOGV2(20083, "message", attrs);

    validate([](const BSONObj& obj) {
        const BSONObj& attrObj = obj.getField(kAttributesFieldName).Obj();
        for (StringData f : {"cstr"_sd,
                             "int"_sd,
                             "float"_sd,
                             "bool"_sd,
                             "enum"_sd,
                             "custom"_sd,
                             "bson"_sd,
                             "millisMillis"_sd,
                             "stdstr"_sd,
                             "unsafe but ok"_sd}) {
            ASSERT(attrObj.hasField(f));
        }

        // Check that one of them actually has the value too.
        ASSERT_EQUALS(attrObj.getField("int").Int(), 5);
    });
}


struct A {
    std::string toString() const {
        return "A";
    }

    friend auto logAttrs(const A& a) {
        return "a"_attr = a;
    }
};

TEST_F(LogV2JsonBsonTest, AttrWrapperOne) {
    A a;
    LOGV2(4759400, "{}", logAttrs(a));
    validate([&a](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("a").String(),
                      a.toString());
    });
}

struct B {
    std::string toString() const {
        return "B";
    }
    friend auto logAttrs(const B& b) {
        return "b"_attr = b;
    }
};

TEST_F(LogV2JsonBsonTest, AttrWrapperTwo) {
    A a;
    B b;
    LOGV2(4759401, "{}", logAttrs(a), logAttrs(b));
    validate([&a, &b](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("a").String(),
                      a.toString());
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("b").String(),
                      b.toString());
    });
}

struct C {
    std::string toString() const {
        return "C";
    }
    friend auto logAttrs(const C& c) {
        return "c"_attr = c;
    }
};

TEST_F(LogV2JsonBsonTest, AttrWrapperRvalue) {
    A a;
    B b;
    LOGV2(4759402, "{}", logAttrs(a), logAttrs(b), logAttrs(C()));
    validate([&a, &b](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("a").String(),
                      a.toString());
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("b").String(),
                      b.toString());
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("c").String(),
                      C().toString());
    });
}

struct D {
    std::string toString() const {
        return "D";
    }

    A a() const {
        return A();
    }
    const B& b() const {
        return _b;
    }

    friend auto logAttrs(const D& d) {
        return multipleAttrs("d"_attr = d, d.a(), d.b());
    }

    B _b;
};

TEST_F(LogV2JsonBsonTest, AttrWrapperComplex) {
    D d;
    LOGV2(4759403, "{}", logAttrs(d));
    validate([&d](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("a").String(),
                      d.a().toString());
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("b").String(),
                      d.b().toString());
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("d").String(),
                      d.toString());
    });
}

struct E {
    D d() const {
        return D();
    }
    const C& c() const {
        return _c;
    }

    friend auto logAttrs(const E& e) {
        return multipleAttrs(e.d(), e.c());
    }

    C _c;
};

TEST_F(LogV2JsonBsonTest, AttrWrapperComplexHierarchy) {
    E e;
    LOGV2(4759404, "{}", logAttrs(e));
    validate([&e](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("a").String(),
                      e.d().a().toString());
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("b").String(),
                      e.d().b().toString());
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("c").String(),
                      e.c().toString());
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("d").String(),
                      e.d().toString());
    });
}

class LogV2ContainerTest : public LogV2TypesTest {
public:
    using LogV2TypesTest::LogV2TypesTest;

    // Helper to create a comma separated list of a container, stringify is function on how to
    // transform element into a string.
    template <typename It, typename Func>
    static std::string textJoin(It first, It last, Func stringify) {
        std::string r;
        r += "(";
        const char* sep = "";
        for (; first != last; ++first) {
            r += sep;
            r += stringify(*first);
            sep = ", ";
        }
        r += ")";
        return r;
    }

    template <typename Container, typename Func>
    static std::string textJoin(const Container& c, Func stringify) {
        using std::begin;
        using std::end;
        return textJoin(begin(c), end(c), stringify);
    }

    /** Ensure json and bson modes both pass. */
    template <typename F>
    void validate(F validator) {
        validator(mongo::fromjson(json->back()));
        validator(BSONObj(bson->back().data()));
    }
};

// All standard sequential containers are supported
TEST_F(LogV2ContainerTest, StandardSequential) {
    std::vector<std::string> vectorStrings = {"str1", "str2", "str3"};
    LOGV2(20047, "{name}", "name"_attr = vectorStrings);
    ASSERT_EQUALS(text->back(), textJoin(vectorStrings, [](auto&& s) { return s; }));
    validate([&vectorStrings](const BSONObj& obj) {
        std::vector<BSONElement> jsonVector =
            obj.getField(kAttributesFieldName).Obj().getField("name").Array();
        ASSERT_EQUALS(vectorStrings.size(), jsonVector.size());
        for (std::size_t i = 0; i < vectorStrings.size(); ++i)
            ASSERT_EQUALS(jsonVector[i].String(), vectorStrings[i]);
    });
}

// Test that containers can contain uint32_t, even as this type is not BSON appendable
TEST_F(LogV2ContainerTest, Uint32Sequence) {
    std::vector<uint32_t> vectorUInt32s = {0, 1, std::numeric_limits<uint32_t>::max()};
    LOGV2(4684000, "{vectorUInt32s}", "vectorUInt32s"_attr = vectorUInt32s);
    validate([&vectorUInt32s](const BSONObj& obj) {
        std::vector<BSONElement> jsonVector =
            obj.getField(kAttributesFieldName).Obj().getField("vectorUInt32s").Array();
        ASSERT_EQUALS(vectorUInt32s.size(), jsonVector.size());
        for (std::size_t i = 0; i < vectorUInt32s.size(); ++i) {
            const auto& jsonElem = jsonVector[i];
            if (jsonElem.type() == BSONType::numberInt)
                ASSERT_EQUALS(jsonElem.Int(), vectorUInt32s[i]);
            else if (jsonElem.type() == BSONType::numberLong)
                ASSERT_EQUALS(jsonElem.Long(), vectorUInt32s[i]);
            else
                ASSERT(false) << "Element type is " << typeName(jsonElem.type())
                              << ". Expected Int or Long.";
        }
    });
}

// Elements can require custom formatting
TEST_F(LogV2ContainerTest, CustomFormatting) {
    std::list<TypeWithBSON> listCustom = {
        TypeWithBSON(0.0, 1.0), TypeWithBSON(2.0, 3.0), TypeWithBSON(4.0, 5.0)};
    LOGV2(20048, "{name}", "name"_attr = listCustom);
    ASSERT_EQUALS(text->back(), textJoin(listCustom, [](auto&& x) { return x.toString(); }));
    validate([&listCustom](const BSONObj& obj) {
        std::vector<BSONElement> jsonVector =
            obj.getField(kAttributesFieldName).Obj().getField("name").Array();
        ASSERT_EQUALS(listCustom.size(), jsonVector.size());
        auto out = jsonVector.begin();
        for (auto in = listCustom.begin(); in != listCustom.end(); ++in, ++out) {
            ASSERT_EQUALS(in->toBSON().woCompare(out->Obj()), 0);
        }
    });
}

// Optionals are also allowed as elements
TEST_F(LogV2ContainerTest, OptionalsAsElements) {
    std::forward_list<boost::optional<bool>> listOptionalBool = {true, boost::none, false};
    LOGV2(20049, "{name}", "name"_attr = listOptionalBool);
    ASSERT_EQUALS(text->back(), textJoin(listOptionalBool, [](const auto& item) -> std::string {
                      if (!item)
                          return std::string{constants::kNullOptionalString};
                      if (*item)
                          return "true";
                      return "false";
                  }));
    validate([&listOptionalBool](const BSONObj& obj) {
        std::vector<BSONElement> jsonVector =
            obj.getField(kAttributesFieldName).Obj().getField("name").Array();
        auto in = listOptionalBool.begin();
        auto out = jsonVector.begin();
        for (; in != listOptionalBool.end() && out != jsonVector.end(); ++in, ++out) {
            if (*in)
                ASSERT_EQUALS(**in, out->Bool());
            else
                ASSERT(out->isNull());
        }
        ASSERT(in == listOptionalBool.end());
        ASSERT(out == jsonVector.end());
    });
}

// Containers can be nested
TEST_F(LogV2ContainerTest, Nested) {
    std::array<std::deque<int>, 4> arrayOfDeques = {{{0, 1}, {2, 3}, {4, 5}, {6, 7}}};
    LOGV2(20050, "{name}", "name"_attr = arrayOfDeques);
    ASSERT_EQUALS(text->back(), textJoin(arrayOfDeques, [](auto&& outer) {
                      return textJoin(outer, [](auto&& v) { return fmt::format("{}", v); });
                  }));
    validate([&arrayOfDeques](const BSONObj& obj) {
        std::vector<BSONElement> jsonVector =
            obj.getField(kAttributesFieldName).Obj().getField("name").Array();
        ASSERT_EQUALS(arrayOfDeques.size(), jsonVector.size());
        auto out = jsonVector.begin();
        for (auto in = arrayOfDeques.begin(); in != arrayOfDeques.end(); ++in, ++out) {
            std::vector<BSONElement> inner_array = out->Array();
            ASSERT_EQUALS(in->size(), inner_array.size());
            auto inner_begin = in->begin();
            auto inner_end = in->end();

            auto inner_out = inner_array.begin();
            for (; inner_begin != inner_end; ++inner_begin, ++inner_out) {
                ASSERT_EQUALS(*inner_begin, inner_out->Int());
            }
        }
    });
}

TEST_F(LogV2ContainerTest, Associative) {
    // Associative containers are also supported
    std::map<std::string, std::string> mapStrStr = {{"key1", "val1"}, {"key2", "val2"}};
    LOGV2(20051, "{name}", "name"_attr = mapStrStr);
    ASSERT_EQUALS(text->back(), textJoin(mapStrStr, [](const auto& item) {
                      return fmt::format("{}: {}", item.first, item.second);
                  }));
    validate([&mapStrStr](const BSONObj& obj) {
        BSONObj mappedValues = obj.getField(kAttributesFieldName).Obj().getField("name").Obj();
        auto in = mapStrStr.begin();
        for (; in != mapStrStr.end(); ++in) {
            ASSERT_EQUALS(mappedValues.getField(in->first).String(), in->second);
        }
    });
}

// Associative containers with optional sequential container is ok too
TEST_F(LogV2ContainerTest, AssociativeWithOptionalSequential) {
    stdx::unordered_map<std::string, boost::optional<std::vector<int>>> mapOptionalVector = {
        {"key1", boost::optional<std::vector<int>>{{1, 2, 3}}},
        {"key2", boost::optional<std::vector<int>>{boost::none}}};

    LOGV2(20052, "{name}", "name"_attr = mapOptionalVector);
    ASSERT_EQUALS(text->back(), textJoin(mapOptionalVector, [](auto&& item) {
                      std::string r = item.first + ": ";
                      if (item.second) {
                          r += textJoin(*item.second, [](int v) { return fmt::format("{}", v); });
                      } else {
                          r += std::string{constants::kNullOptionalString};
                      }
                      return r;
                  }));
    validate([&mapOptionalVector](const BSONObj& obj) {
        BSONObj mappedValues = obj.getField(kAttributesFieldName).Obj().getField("name").Obj();
        auto in = mapOptionalVector.begin();
        for (; in != mapOptionalVector.end(); ++in) {
            BSONElement mapElement = mappedValues.getField(in->first);
            if (!in->second)
                ASSERT(mapElement.isNull());
            else {
                const std::vector<int>& intVec = *(in->second);
                std::vector<BSONElement> jsonVector = mapElement.Array();
                ASSERT_EQUALS(jsonVector.size(), intVec.size());
                for (std::size_t i = 0; i < intVec.size(); ++i)
                    ASSERT_EQUALS(jsonVector[i].Int(), intVec[i]);
            }
        }
    });
}

TEST_F(LogV2ContainerTest, DurationVector) {
    std::vector<Nanoseconds> nanos = {Nanoseconds(10), Nanoseconds(100)};
    LOGV2(20081, "{name}", "name"_attr = nanos);
    validate([&nanos](const BSONObj& obj) {
        std::vector<BSONElement> jsonVector =
            obj.getField(kAttributesFieldName).Obj().getField("name").Array();
        ASSERT_EQUALS(nanos.size(), jsonVector.size());
        for (std::size_t i = 0; i < nanos.size(); ++i)
            ASSERT(jsonVector[i].Obj().woCompare(nanos[i].toBSON()) == 0);
    });
}

TEST_F(LogV2ContainerTest, DurationMap) {
    std::map<std::string, Microseconds> mapOfMicros = {{"first", Microseconds(20)},
                                                       {"second", Microseconds(40)}};
    LOGV2(20082, "{name}", "name"_attr = mapOfMicros);
    validate([&mapOfMicros](const BSONObj& obj) {
        BSONObj mappedValues = obj.getField(kAttributesFieldName).Obj().getField("name").Obj();
        auto in = mapOfMicros.begin();
        for (; in != mapOfMicros.end(); ++in) {
            ASSERT(mappedValues.getField(in->first).Obj().woCompare(in->second.toBSON()) == 0);
        }
    });
}

TEST_F(LogV2ContainerTest, StringMapUint32) {
    // Test that maps can contain uint32_t, even as this type is not BSON appendable
    StringMap<uint32_t> mapOfUInt32s = {
        {"first", 0}, {"second", 1}, {"third", std::numeric_limits<uint32_t>::max()}};
    LOGV2(4684001, "{mapOfUInt32s}", "mapOfUInt32s"_attr = mapOfUInt32s);
    validate([&mapOfUInt32s](const BSONObj& obj) {
        BSONObj mappedValues =
            obj.getField(kAttributesFieldName).Obj().getField("mapOfUInt32s").Obj();
        for (const auto& mapElem : mapOfUInt32s) {
            auto elem = mappedValues.getField(mapElem.first);
            if (elem.type() == BSONType::numberInt)
                ASSERT_EQUALS(elem.Int(), mapElem.second);
            else if (elem.type() == BSONType::numberLong)
                ASSERT_EQUALS(elem.Long(), mapElem.second);
            else
                ASSERT(false) << "Element type is " << typeName(elem.type())
                              << ". Expected Int or Long.";
        }
    });
}

TEST_F(LogV2Test, AttrNameCollision) {
    ASSERT_THROWS_CODE(LOGV2(4793300, "Collision", "k1"_attr = "v1", "k1"_attr = "v2"),
                       AssertionException,
                       4793301);
}

TEST_F(LogV2Test, Unicode) {
    auto lines = makeLineCapture(JSONFormatter());

    // JSON requires strings to be valid UTF-8 and control characters escaped.
    // JSON parsers decode escape sequences so control characters should be round-trippable.
    // Invalid UTF-8 encoded data is replaced by the Unicode Replacement Character (U+FFFD).
    // There is no way to preserve the data without introducing special semantics in how to parse.
    std::pair<StringData, StringData> strs[] = {
        // Single byte characters that needs to be escaped
        {"\a\b\f\n\r\t\v\\\0\x7f\x1b"_sd, "\a\b\f\n\r\t\v\\\0\x7f\x1b"_sd},
        // multi byte characters that needs to be escaped (unicode control characters)
        {"\u0080\u009f"_sd, "\u0080\u009f"_sd},
        // Valid 2 Octet sequence, LATIN SMALL LETTER N WITH TILDE
        {"\u00f1"_sd, "\u00f1"_sd},
        // Invalid 2 Octet Sequence, result is escaped
        {"\xc3\x28"_sd, "\ufffd\x28"_sd},
        // Invalid Sequence Identifier, result is escaped
        {"\xa0\xa1"_sd, "\ufffd\ufffd"_sd},
        // Valid 3 Octet sequence, RUNIC LETTER TIWAZ TIR TYR T
        {"\u16cf"_sd, "\u16cf"_sd},
        // Invalid 3 Octet Sequence (in 2nd Octet), result is escaped
        {"\xe2\x28\xa1"_sd, "\ufffd\x28\ufffd"_sd},
        // Invalid 3 Octet Sequence (in 3rd Octet), result is escaped
        {"\xe2\x82\x28"_sd, "\ufffd\ufffd\x28"_sd},
        // Valid 4 Octet sequence, GOTHIC LETTER MANNA
        {"\U0001033c"_sd, "\U0001033c"_sd},
        // Invalid 4 Octet Sequence (in 2nd Octet), result is escaped
        {"\xf0\x28\x8c\xbc"_sd, "\ufffd\x28\ufffd\ufffd"_sd},
        // Invalid 4 Octet Sequence (in 3rd Octet), result is escaped
        {"\xf0\x90\x28\xbc"_sd, "\ufffd\ufffd\x28\ufffd"_sd},
        // Invalid 4 Octet Sequence (in 4th Octet), result is escaped
        {"\xf0\x28\x8c\x28"_sd, "\ufffd\x28\ufffd\x28"_sd},
        // Valid 5 Octet Sequence (but not Unicode!), result is escaped
        {"\xf8\xa1\xa1\xa1\xa1"_sd, "\ufffd\ufffd\ufffd\ufffd\ufffd"_sd},
        // Valid 6 Octet Sequence (but not Unicode!), result is escaped
        {"\xfc\xa1\xa1\xa1\xa1\xa1"_sd, "\ufffd\ufffd\ufffd\ufffd\ufffd\ufffd"_sd},
        // Invalid 3 Octet sequence, buffer ends prematurely, result is escaped
        {"\xe2\x82"_sd, "\ufffd\ufffd"_sd},
    };

    auto getLastMongo = [&]() {
        return mongo::fromjson(lines->back())
            .getField(constants::kAttributesFieldName)
            .Obj()
            .getField("name")
            .String();
    };

    auto getLastPtree = [&]() {
        namespace pt = boost::property_tree;

        std::istringstream json_stream(lines->back());
        pt::ptree ptree;
        pt::json_parser::read_json(json_stream, ptree);
        return ptree.get<std::string>(std::string(constants::kAttributesFieldName) + ".name");
    };

    for (const auto& pair : strs) {
        LOGV2(20053, "{name}", "name"_attr = pair.first);

        // Verify with both our parser and boost::property_tree
        ASSERT_EQUALS(pair.second, getLastMongo());
        ASSERT_EQUALS(pair.second, getLastPtree());
    }
}

class LogV2JsonTruncationTest : public LogV2Test {
public:
    static constexpr std::size_t maxAttributeOutputSize =
        constants::kDefaultMaxAttributeOutputSizeKB * 1024;

    static inline const std::string largeString = std::string(maxAttributeOutputSize * 2, 'a');

    // Represents a segment of the truncation path
    struct TruncationPathSegment {
        // name of the field where the truncation occurs
        std::string fieldName;

        // number of omitted fields after fieldName
        int omitted;
    };

    struct TruncationInfo {
        // Truncation path is a sequence of segments for each level of the truncated BSON object,
        // where the index in the sequence is the same as the depth of that segment in the object.
        // The last segment in the path is the "leaf" from which the truncation started,
        // and unlike the segments before it, it is NOT expected to appear in the truncated object,
        // and must therefore be counted in the expected "omitted" value.
        std::vector<TruncationPathSegment> path;

        // BSON type of the leaf element from which the truncation started
        BSONType leafType;
    };

    struct TestCase {
        // Attr object before truncation
        BSONObj originalDoc;

        // Describes the expected truncation of the attr object
        TruncationInfo truncationInfo;

        // Name for this test case
        std::string name;
    };

    static TestCase largeStringInSubobjTest() {
        BSONObjBuilder builder;
        TruncationInfo truncation;
        builder.append("lvl1_a", "a");
        {
            BSONObjBuilder subobj1 = builder.subobjStart("sub1"_sd);
            subobj1.append("lvl2_a", 1);
            subobj1.append("lvl2_b", "small string");
            {
                BSONObjBuilder subobj2 = subobj1.subobjStart("sub2"_sd);
                subobj2.append("lvl3_a", 1);
                subobj2.append("lvl3_b", "b");
                subobj2.append("large", largeString);
                subobj2.append("lvl3_c", "small string after large object");
            }
            subobj1.append("lvl2_c", 1);
            subobj1.append("lvl2_d", 2);
        }
        truncation.leafType = BSONType::string;
        truncation.path = {{"sub1", 0}, {"sub2", 2}, {"large", 2}};
        return TestCase{builder.obj(), std::move(truncation), "large string in subobject"};
    }

    static TestCase singleLargeStringInObjTest() {
        BSONObjBuilder builder;
        TruncationInfo truncation;
        builder.append("large", largeString);
        truncation.leafType = BSONType::string;
        truncation.path = {{"large", 1}};
        return TestCase{builder.obj(), std::move(truncation), "single large string in object"};
    }

    static TestCase largeArrayTest() {
        BSONArrayBuilder builder;
        TruncationInfo truncation;
        for (size_t i = 0; i < maxAttributeOutputSize; ++i) {
            builder.append("str");
        }
        truncation.leafType = BSONType::string;
        truncation.path = {{"862", maxAttributeOutputSize - 862}};
        return TestCase{builder.arr(), std::move(truncation), "large array"};
    }

    static TestCase singleLargeStringInArrayTest() {
        BSONArrayBuilder builder;
        TruncationInfo truncation;
        builder.append(largeString);
        truncation.leafType = BSONType::string;
        truncation.path = {{"0", 1}};
        return TestCase{builder.arr(), std::move(truncation), "single large string in array"};
    }

    static TestCase largeStringInNestedArraysTest() {
        BSONArrayBuilder builder;
        TruncationInfo truncation;
        builder.append("1_a");  // ["1_a",
        {
            // ["1_a", [
            BSONArrayBuilder subarr1 = builder.subarrayStart();
            // ["1_a", [[
            BSONArrayBuilder{subarr1.subarrayStart()}
                .append("3_a")
                .append("3_b")
                .append("3_c")
                .append(largeString)
                .append("3_d");
        }
        builder.append("1_b");
        // ["1_a", [["3_a", "3_b", "3_c", largeString, "3_d"]], "1_b"]
        auto array = builder.arr();

        truncation.leafType = BSONType::string;
        truncation.path = {{"1", 1}, {"0", 0}, {"3", 2}};
        return TestCase{array, std::move(truncation), "large string in nested arrays"};
    }

    static std::vector<TestCase> generateTests() {
        return {largeStringInSubobjTest(),
                singleLargeStringInObjTest(),
                largeArrayTest(),
                singleLargeStringInArrayTest(),
                largeStringInNestedArraysTest()};
    }

    // Validates the truncation report in the log line for attrName has the correct structure
    // that matches the expected truncation path.
    // For reference, an example truncation report (for largeStringInSubobj test case) looks like:
    //  {
    //     "truncated":{
    //         "sub1":{
    //             "truncated":{
    //                 "sub2":{
    //                     "truncated":{
    //                         "large":{
    //                             "type":"string",
    //                             "size":{"$numberInt":"20485"}
    //                         }
    //                     },
    //                     "omitted":{"$numberInt":"2"}
    //                 }
    //             },
    //             "omitted":{"$numberInt":"2"}
    //         }
    //     }
    //  }
    static void validateTruncationReport(StringData attrName,
                                         BSONObj report,
                                         const TestCase& test) {
        auto context =
            fmt::format("Failed test: {} Failing report: {}", test.name, mongo::tojson(report));
        auto& path = test.truncationInfo.path;

        ASSERT_FALSE(path.empty()) << context;
        ASSERT(report.hasField(attrName)) << context;

        BSONObj fieldObj = report.getField(attrName).Obj();
        BSONObj truncated;
        std::string currentObjPath = std::string{attrName};

        // validate nested "truncated" elements except for the last (leaf) truncated element.
        for (size_t i = 0; i < path.size(); i++) {
            const auto& segment = path.at(i);

            ASSERT(fieldObj.hasField(constants::kTruncatedFieldName)) << fmt::format(
                "{} - missing 'truncated' field at path: {}", context, currentObjPath);

            truncated = fieldObj.getField(constants::kTruncatedFieldName).Obj();

            if (segment.omitted != 0) {
                ASSERT(fieldObj.hasField(constants::kOmittedFieldName)) << fmt::format(
                    "{} - missing 'omitted' field at path: {}", context, currentObjPath);
                ASSERT_EQUALS(fieldObj.getField("omitted").Int(), segment.omitted)
                    << fmt::format("{} - bad 'omitted' value at path: {}", context, currentObjPath);
            } else {
                ASSERT_FALSE(fieldObj.hasField("omitted")) << fmt::format(
                    "{} - unexpected 'omitted' field at path: {}", context, currentObjPath);
            }

            currentObjPath += ".truncated";
            ASSERT(truncated.hasField(segment.fieldName))
                << fmt::format("{} - missing expected subobject {} at path {}",
                               context,
                               segment.fieldName,
                               currentObjPath);

            fieldObj = truncated.getField(segment.fieldName).Obj();
            currentObjPath += "." + segment.fieldName;
        }
        // leaf reached
        ASSERT(fieldObj.hasField("type"))
            << fmt::format("{} - missing field 'type' at path {}", context, currentObjPath);

        ASSERT(fieldObj.hasField("size"))
            << fmt::format("{} - missing field 'size' at path {}", context, currentObjPath);

        ASSERT(!fieldObj.hasField("omitted"))
            << fmt::format("{} - unexpected field 'omitted' at path {}", context, currentObjPath);

        ASSERT(!fieldObj.hasField("truncated"))
            << fmt::format("{} - unexpected field 'truncated' at path {}", context, currentObjPath);

        ASSERT_EQUALS(fieldObj.getField("type"_sd).String(), typeName(test.truncationInfo.leafType))
            << fmt::format("{} - bad 'type' value at path {}", context, currentObjPath);

        ASSERT(fieldObj.getField("size"_sd).isNumber())
            << fmt::format("{} - bad 'size' value at path {}", context, currentObjPath);
    }

    // Validates the reported size of the truncated attr in the log line matches the size of the
    // original BSON object.
    static void validateTruncationSize(StringData attrName,
                                       BSONObj truncatedSize,
                                       const TestCase& test) {
        auto context = fmt::format(
            "Failed test: {} Failing report: {}", test.name, mongo::tojson(truncatedSize));
        ASSERT(truncatedSize.hasField(attrName)) << context;
        auto reportedSize = truncatedSize.getField(attrName).Int();
        auto expectedSize = test.originalDoc.objsize();
        ASSERT_EQUALS(reportedSize, expectedSize) << context;
    }

    // At every level of the modified document, validates that only the fields before & including
    // the truncation path are present.
    static void validateTruncationAtPath(const BSONObj& original,
                                         const BSONObj& modified,
                                         const TestCase& test,
                                         const std::string& parentPath,
                                         size_t level) {
        static const SimpleBSONElementComparator eltCmp;
        auto context = fmt::format("Failed test: {} Path: {}", test.name, parentPath);

        auto& path = test.truncationInfo.path;

        ASSERT_LT(level, path.size());

        BSONObjIterator originalItr(original);
        BSONObjIterator modifiedItr(modified);
        bool foundTruncatedElement = false;

        StringData truncatedFieldName = path.at(level).fieldName;
        bool leaf = (&path.at(level) == &path.back());

        while (originalItr.more() && modifiedItr.more()) {
            auto originalElement = originalItr.next();
            auto modifiedElement = modifiedItr.next();

            ASSERT_EQUALS(modifiedElement.fieldNameStringData(),
                          originalElement.fieldNameStringData())
                << fmt::format("{} - mismatched field names {} vs {}",
                               context,
                               originalElement.fieldNameStringData(),
                               modifiedElement.fieldNameStringData());

            if (originalElement.fieldNameStringData() == truncatedFieldName) {
                foundTruncatedElement = true;

                // if truncatedFieldName is present in the truncated object, but the test expects
                // it to be a leaf, then it should have been omitted.
                ASSERT_FALSE(leaf)
                    << fmt::format("{} - unexpected field {}", context, truncatedFieldName);

                ASSERT_FALSE(modifiedItr.more()) << fmt::format(
                    "{} - truncation did not stop at field {}", context, truncatedFieldName);

                ASSERT(modifiedElement.isABSONObj())
                    << fmt::format("{} - unexpected leaf element {}", context, truncatedFieldName);

                validateTruncationAtPath(originalElement.Obj(),
                                         modifiedElement.Obj(),
                                         test,
                                         fmt::format("{}.{}", parentPath, truncatedFieldName),
                                         level + 1);
            } else {
                ASSERT(eltCmp.evaluate(originalElement == modifiedElement))
                    << context << " - mismatched field values at "
                    << originalElement.fieldNameStringData();
            }
        }
        if (originalItr.more() && !foundTruncatedElement) {
            // if the original object has more fields than the modified object, but the truncated
            // field name is not in the modified object, then it MUST have been an omitted leaf
            // element.
            ASSERT(leaf) << fmt::format(
                "{} - missing truncated field {}", context, truncatedFieldName);

            // The next element in the original object must be the truncated field name
            auto nextElement = originalItr.next();
            ASSERT_EQUALS(nextElement.fieldNameStringData(), truncatedFieldName)
                << fmt::format("{} - unexpected field {}, expected {}",
                               context,
                               nextElement.fieldNameStringData(),
                               truncatedFieldName);

            foundTruncatedElement = true;
        }
        ASSERT(foundTruncatedElement)
            << fmt::format("{} - missing truncated field {}", context, truncatedFieldName);
    }

    static void validateTruncatedAttr(const TestCase& test, const BSONObj& truncatedAttr) {
        validateTruncationAtPath(test.originalDoc, truncatedAttr, test, "", 0);
    }
};

TEST_F(LogV2JsonTruncationTest, JsonTruncation) {
    auto lines = makeLineCapture(JSONFormatter());

    for (auto& test : generateTests()) {
        LOGV2(20085, "message", "name"_attr = test.originalDoc, "attr2"_attr = true);
        auto logObj = fromjson(lines->back());
        auto loggedAttr =
            logObj.getField(constants::kAttributesFieldName).Obj().getField("name").Obj();

        // Check that all fields up until the large one is written
        validateTruncatedAttr(test, loggedAttr);

        auto report = logObj.getField(constants::kTruncatedFieldName).Obj();
        validateTruncationReport("name", report, test);

        auto size = logObj.getField(constants::kTruncatedSizeFieldName).Obj();
        validateTruncationSize("name", size, test);

        // Attributes coming after the truncated one should be written
        ASSERT(logObj.getField(constants::kAttributesFieldName).Obj().getField("attr2").Bool());
    }
}

TEST_F(LogV2JsonTruncationTest, JsonTruncationDisabled) {
    auto lines = makeLineCapture(JSONFormatter());

    for (auto& test : generateTests()) {
        LOGV2_OPTIONS(20086, {LogTruncation::Disabled}, "message", "name"_attr = test.originalDoc);
        auto logObj = fromjson(lines->back());
        auto loggedAttr =
            logObj.getField(constants::kAttributesFieldName).Obj().getField("name").Obj();

        ASSERT_EQUALS(loggedAttr.woCompare(test.originalDoc), 0);
        ASSERT_FALSE(logObj.hasField(constants::kTruncatedFieldName));
        ASSERT_FALSE(logObj.hasField(constants::kTruncatedSizeFieldName));
    }
}

TEST_F(LogV2Test, StringTruncation) {
    const AtomicWord<int32_t> maxAttributeSizeKB(1);
    auto lines = makeLineCapture(JSONFormatter(&maxAttributeSizeKB));

    std::size_t maxLength = maxAttributeSizeKB.load() << 10;
    std::string prefix(maxLength - 3, 'a');

    struct TestCase {
        std::string input;
        std::string suffix;
        std::string note;
    };

    TestCase tests[] = {
        {prefix + "LMNOPQ", "LMN", "unescaped 1-byte octet"},
        // "\n\"NOPQ" expands to "\\n\\\"NOPQ" after escape, and the limit
        // is reached at the 2nd '\\' octet, but since it splits the "\\\""
        // sequence, the actual truncation happens after the 'n' octet.
        {prefix + "\n\"NOPQ", "\n", "2-byte escape sequence"},
        // "L\vNOPQ" expands to "L\\u000bNOPQ" after escape, and the limit
        // is reached at the 'u' octet, so the entire sequence is truncated.
        {prefix + "L\vNOPQ", "L", "multi-byte escape sequence"},
        {prefix + "LM\xC3\xB1PQ", "LM", "2-byte UTF-8 sequence"},
        {prefix + "L\xE1\x9B\x8FPQ", "L", "3-byte UTF-8 sequence"},
        {prefix + "L\xF0\x90\x8C\xBCQ", "L", "4-byte UTF-8 sequence"},
        {prefix + "\xE1\x9B\x8E\xE1\x9B\x8F", "\xE1\x9B\x8E", "UTF-8 codepoint boundary"},
        // The invalid UTF-8 codepoint 0xC3 is replaced with "\\ufffd", and truncated entirely
        {prefix + "L\xC3NOPQ", "L", "escaped invalid codepoint"},
        {std::string(maxLength, '\\'), "\\", "escaped backslash"},
    };

    for (const auto& [input, suffix, note] : tests) {
        LOGV2(6694001, "name", "name"_attr = input);
        BSONObj obj = fromjson(lines->back());

        auto str = obj[constants::kAttributesFieldName]["name"].checkAndGetStringData();
        std::string context = "Failed test: " + note;

        ASSERT_LTE(str.size(), maxLength) << context;
        ASSERT(str.ends_with(suffix))
            << context << " - string " << str << " does not end with " << suffix;

        auto trunc = obj[constants::kTruncatedFieldName]["name"];
        ASSERT_EQUALS(trunc["type"].String(), typeName(BSONType::string)) << context;
        ASSERT_EQUALS(trunc["size"].numberLong(), str::escapeForJSON(input).size()) << context;
    }
}

TEST_F(LogV2Test, Threads) {
    auto linesPlain = makeLineCapture(PlainFormatter());
    auto linesText = makeLineCapture(TextFormatter());
    auto linesJson = makeLineCapture(JSONFormatter());

    constexpr int kNumPerThread = 1000;
    std::vector<stdx::thread> threads;

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2(20054, "thread1");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2(20055, "thread2");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2(20056, "thread3");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2(20057, "thread4");
    });

    for (auto&& thread : threads) {
        thread.join();
    }

    ASSERT(linesPlain->size() == threads.size() * kNumPerThread);
    ASSERT(linesText->size() == threads.size() * kNumPerThread);
    ASSERT(linesJson->size() == threads.size() * kNumPerThread);
}

TEST_F(LogV2Test, Ramlog) {
    RamLog* ramlog = RamLog::get("test_ramlog");
    auto sink = wrapInUnlockedSink(boost::make_shared<RamLogSink>(ramlog));
    applyDefaultFilterToSink(sink);
    sink->set_formatter(PlainFormatter());
    attachSink(sink);

    auto lines = makeLineCapture(PlainFormatter(), false);

    auto verifyRamLog = [&] {
        RamLog::LineIterator iter(ramlog);
        for (const auto& s : lines->lines()) {
            const auto next = iter.next();
            if (s != next) {
                std::cout << "\n\n\n********************** s='" << s << "', next='" << next
                          << "'\n";
                return false;
            }
        }
        return true;
    };

    LOGV2(20058, "test");
    ASSERT(verifyRamLog());
    LOGV2(20059, "test2");
    ASSERT(verifyRamLog());
}

TEST_F(LogV2Test, Ramlog_AltMaxLinesMaxSize) {
    constexpr size_t alternativeMaxLines = 2048;
    constexpr size_t alternativeMaxSizeBytes = 2 * 1024 * 1024;
    RamLog* ramlog = RamLog::get("test_ramlog_alt2", alternativeMaxLines, alternativeMaxSizeBytes);
    auto sink = wrapInUnlockedSink(boost::make_shared<RamLogSink>(ramlog));
    applyDefaultFilterToSink(sink);
    sink->set_formatter(PlainFormatter());
    attachSink(sink);

    auto lines = makeLineCapture(PlainFormatter(), false);

    auto verifyRamLog = [&] {
        RamLog::LineIterator iter(ramlog);
        for (const auto& s : lines->lines()) {
            const auto next = iter.next();
            if (s != next) {
                std::cout << "\n\n\n********************** s='" << s << "', next='" << next
                          << "'\n";
                return false;
            }
        }
        return true;
    };

    LOGV2(5816501, "test");
    ASSERT(verifyRamLog());
    LOGV2(5816502, "test2");
    ASSERT(verifyRamLog());
}

// Positive: Test that the ram log is properly circular
TEST_F(LogV2Test, Ramlog_CircularBuffer) {
    RamLog* ramlog = RamLog::get("test_ramlog2");

    std::vector<std::string> lines;

    constexpr size_t maxLines = 1024;
    constexpr size_t testLines = 5000;

    // Write enough lines to trigger wrapping
    for (size_t i = 0; i < testLines; ++i) {
        auto s = std::to_string(i);
        lines.push_back(s);
        ramlog->write(s);
    }

    lines.erase(lines.begin(), lines.begin() + (testLines - maxLines) + 1);

    // Verify we circled correctly through the buffer
    {
        RamLog::LineIterator iter(ramlog);
        ASSERT_EQ(iter.getTotalLinesWritten(), 5000UL);
        for (const auto& line : lines) {
            ASSERT_EQ(line, iter.next());
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log is properly circular
TEST_F(LogV2Test, Ramlog_CircularBuffer_AltMaxLinesMaxSize) {
    constexpr size_t alternativeMaxLines = 10;
    constexpr size_t alternativeMaxSizeBytes = 2 * 1024 * 1024;
    RamLog* ramlog = RamLog::get("test_ramlog2_alt2", alternativeMaxLines, alternativeMaxSizeBytes);
    ASSERT_EQ(alternativeMaxLines, ramlog->getMaxLines());
    ASSERT_EQ(alternativeMaxSizeBytes, ramlog->getMaxSizeBytes());

    std::vector<std::string> lines;

    constexpr size_t maxLines = alternativeMaxLines;
    constexpr size_t testLines = 12;

    // Write enough lines to trigger wrapping
    for (size_t i = 0; i < testLines; ++i) {
        auto s = std::to_string(i);
        lines.push_back(s);
        ramlog->write(s);
    }

    lines.erase(lines.begin(), lines.begin() + (testLines - maxLines) + 1);

    // Verify we circled correctly through the buffer
    {
        RamLog::LineIterator iter(ramlog);
        ASSERT_EQ(iter.getTotalLinesWritten(), testLines);
        int n = 1;
        for (const auto& line : lines) {
            ASSERT_EQ(line, iter.next()) << "\n\n\n   n=" << n << "\n\n\n\n";
            n++;
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log has a max size cap
TEST_F(LogV2Test, Ramlog_MaxSize) {
    RamLog* ramlog = RamLog::get("test_ramlog3");

    std::vector<std::string> lines;

    constexpr size_t testLines = 2000;
    constexpr size_t longStringLength = 2048;

    std::string longStr(longStringLength, 'a');

    // Write enough lines to trigger wrapping and trimming
    for (size_t i = 0; i < testLines; ++i) {
        auto s = std::to_string(10000 + i) + longStr;
        lines.push_back(s);
        ramlog->write(s);
    }

    constexpr size_t linesToFit = (1024 * 1024) / (5 + longStringLength);

    lines.erase(lines.begin(), lines.begin() + (testLines - linesToFit));

    // Verify we keep just enough lines that fit
    {
        RamLog::LineIterator iter(ramlog);
        ASSERT_EQ(iter.getTotalLinesWritten(), 2000UL);
        for (const auto& line : lines) {
            ASSERT_EQ(line, iter.next());
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log has a max size cap
TEST_F(LogV2Test, Ramlog_MaxSize_AltMaxLinesMaxSize) {
    constexpr size_t testLines = 2000;
    constexpr size_t longStringLength = 2048;
    constexpr size_t fullStringLength = 2048 + 5;

    constexpr size_t alternativeMaxLines = 2048;
    constexpr size_t alternativeMaxSizeBytes = 1024 * 1024 + fullStringLength;
    RamLog* ramlog = RamLog::get("test_ramlog3_alt", alternativeMaxLines, alternativeMaxSizeBytes);

    std::vector<std::string> lines;

    std::string longStr(longStringLength, 'a');

    // Write enough lines to trigger wrapping and trimming
    for (size_t i = 0; i < testLines; ++i) {
        auto s = std::to_string(10000 + i) + longStr;
        lines.push_back(s);
        ramlog->write(s);
    }

    constexpr size_t linesToFit = alternativeMaxSizeBytes / fullStringLength;

    lines.erase(lines.begin(), lines.begin() + (testLines - linesToFit));

    // Verify we keep just enough lines that fit
    {
        RamLog::LineIterator iter(ramlog);
        ASSERT_EQ(iter.getTotalLinesWritten(), 2000UL);
        for (const auto& line : lines) {
            ASSERT_EQ(line, iter.next());
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log handles really large lines
TEST_F(LogV2Test, Ramlog_GiantLine) {
    RamLog* ramlog = RamLog::get("test_ramlog4");

    std::vector<std::string> lines;

    constexpr size_t testLines = 5000;

    // Write enough lines to trigger wrapping
    for (size_t i = 0; i < testLines; ++i) {
        ramlog->write(std::to_string(i));
    }

    auto s = std::to_string(testLines);
    lines.push_back(s);
    ramlog->write(s);

    std::string bigStr(2048 * 1024, 'a');
    lines.push_back(bigStr);
    ramlog->write(bigStr);

    // Verify we keep 2 lines
    {
        RamLog::LineIterator iter(ramlog);
        ASSERT_EQ(iter.getTotalLinesWritten(), testLines + 2);
        for (const auto& line : lines) {
            ASSERT_EQ(line, iter.next());
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log handles really large lines
TEST_F(LogV2Test, Ramlog_GiantLine_AltMaxLinesMaxSize) {
    constexpr size_t alternativeMaxLines = 1024;
    constexpr size_t alternativeMaxSizeBytes = 2 * 1024 * 1024;
    RamLog* ramlog = RamLog::get("test_ramlog4_alt", alternativeMaxLines, alternativeMaxSizeBytes);

    std::vector<std::string> lines;

    constexpr size_t testLines = 5000;

    // Write enough lines to trigger wrapping
    for (size_t i = 0; i < testLines; ++i) {
        ramlog->write(std::to_string(i));
    }

    auto s = std::to_string(testLines);
    lines.push_back(s);
    ramlog->write(s);

    std::string bigStr(2048 * 1024 + 128, 'a');
    lines.push_back(bigStr);
    ramlog->write(bigStr);

    // Verify we keep 2 lines
    {
        RamLog::LineIterator iter(ramlog);
        ASSERT_EQ(iter.getTotalLinesWritten(), testLines + 2);
        for (const auto& line : lines) {
            ASSERT_EQ(line, iter.next());
        }
    }

    ramlog->clear();
}

TEST_F(LogV2Test, MultipleDomains) {
    // Example how a second domain can be created.
    struct OtherDomain : LogDomain::Internal {
        LogSource& source() override {
            thread_local LogSource lg(this);
            return lg;
        }
    };
    LogDomain other_domain(std::make_unique<OtherDomain>());
    synchronized_value<std::vector<std::string>> other_lines;
    auto other_sink = LogCaptureBackend::create(std::make_unique<Listener>(&other_lines), true);
    other_sink->set_filter(ComponentSettingsFilter(other_domain, mgr().getGlobalSettings()));
    other_sink->set_formatter(PlainFormatter());
    attachSink(other_sink);

    auto global_lines = makeLineCapture(PlainFormatter());

    LOGV2_OPTIONS(20070, {&other_domain}, "test");
    auto logLinesLockGuard = *other_lines;
    ASSERT(global_lines->lines().empty());
    ASSERT(logLinesLockGuard->back() == "test");

    LOGV2(20060, "global domain log");
    ASSERT(global_lines->back() == "global domain log");
    ASSERT(logLinesLockGuard->back() == "test");
}

TEST_F(LogV2Test, FileLogging) {
    auto logv2_dir = std::make_unique<mongo::unittest::TempDir>("logv2");

    // Examples of some capabilities for file logging. Rotation, header/footer support.
    std::string file_name = logv2_dir->path() + "/file.log";
    std::string rotated_file_name = logv2_dir->path() + "/file-rotated.log";

    auto backend = boost::make_shared<boost::log::sinks::text_file_backend>(
        boost::log::keywords::file_name = file_name);
    backend->auto_flush();
    backend->set_open_handler(
        [](boost::log::sinks::text_file_backend::stream_type& file) { file << "header\n"; });
    backend->set_close_handler(
        [](boost::log::sinks::text_file_backend::stream_type& file) { file << "footer\n"; });

    auto sink = wrapInSynchronousSink(backend);
    applyDefaultFilterToSink(sink);
    sink->set_formatter(PlainFormatter());
    attachSink(sink);

    auto readFile = [&](std::string const& filename) {
        std::vector<std::string> lines;
        std::ifstream file(filename);
        for (std::string line; std::getline(file, line, '\n');)
            lines.push_back(std::move(line));
        return lines;
    };

    LOGV2(20061, "test");
    ASSERT(readFile(file_name).back() == "test");

    LOGV2(20062, "test2");
    ASSERT(readFile(file_name).back() == "test2");

    auto before_rotation = readFile(file_name);
    ASSERT(before_rotation.front() == "header");
    if (auto locked = sink->locked_backend()) {
        locked->set_target_file_name_pattern(rotated_file_name);
        locked->rotate_file();
    }

    ASSERT(readFile(file_name).empty());
    auto after_rotation = readFile(rotated_file_name);
    ASSERT(after_rotation.back() == "footer");
    before_rotation.push_back(after_rotation.back());
    ASSERT(before_rotation == after_rotation);
}

TEST_F(LogV2Test, UserAssert) {
    synchronized_value<std::vector<std::string>> syncedLines;
    auto sink = wrapInSynchronousSink(wrapInCompositeBackend(
        boost::make_shared<LogCaptureBackend>(std::make_unique<Listener>(&syncedLines), true),
        boost::make_shared<UserAssertSink>()));
    applyDefaultFilterToSink(sink);
    sink->set_formatter(PlainFormatter());
    attachSink(sink);

    // Depending on verbosity set the assertion code may emit additional log messages after ours,
    // disregard them when verifying by clearing lines after every test
    ASSERT_THROWS_WITH_CHECK(
        LOGV2_OPTIONS(4652000, {UserAssertAfterLog(ErrorCodes::BadValue)}, "uasserting log"),
        DBException,
        [&syncedLines](const DBException& ex) {
            ASSERT_EQUALS(ex.code(), ErrorCodes::BadValue);
            ASSERT_EQUALS(ex.reason(), "uasserting log");
            ASSERT_EQUALS((**syncedLines).front(), ex.reason());
        });
    (**syncedLines).clear();

    ASSERT_THROWS_WITH_CHECK(LOGV2_OPTIONS(4652001,
                                           {UserAssertAfterLog(ErrorCodes::BadValue)},
                                           "uasserting log {name}",
                                           "name"_attr = 1),
                             DBException,
                             [&syncedLines](const DBException& ex) {
                                 ASSERT_EQUALS(ex.code(), ErrorCodes::BadValue);
                                 ASSERT_EQUALS(ex.reason(), "uasserting log 1");
                                 ASSERT_EQUALS((**syncedLines).front(), ex.reason());
                             });
    (**syncedLines).clear();

    ASSERT_THROWS_WITH_CHECK(LOGV2_OPTIONS(4716000, {UserAssertAfterLog()}, "uasserting log"),
                             DBException,
                             [&syncedLines](const DBException& ex) {
                                 ASSERT_EQUALS(ex.code(), 4716000);
                                 ASSERT_EQUALS(ex.reason(), "uasserting log");
                                 ASSERT_EQUALS((**syncedLines).front(), ex.reason());
                             });
}


TEST_F(LogV2Test, EmptyBSONElement) {
    BSONObj doc = BSON("id" << 123);
    ASSERT_DOES_NOT_THROW(
        LOGV2(9189100, "Should not throw", "elem"_attr = doc["nonexistentField"]));
}

class UnstructuredLoggingTest : public LogV2JsonBsonTest {};

TEST_F(UnstructuredLoggingTest, NoArgs) {
    std::string message = "no arguments";
    logd(message);  // NOLINT
    validate([&message](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), message);
    });
}

TEST_F(UnstructuredLoggingTest, Args) {
    std::string format_str = "format {} str {} fields";
    logd(format_str, 1, "str");  // NOLINT
    validate([&format_str](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(),
                      fmt::format(fmt::runtime(format_str), 1, "str"));
    });
}

TEST_F(UnstructuredLoggingTest, ArgsLikeFormatSpecifier) {
    // Ensure the plain formatter does not process the formatted string
    unittest::LogCaptureGuard logs;

    std::string format_str = "format {} str {} fields";
    logd(format_str, 1, "{ x : 1}");  // NOLINT
    validate([&format_str](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(),
                      fmt::format(fmt::runtime(format_str), 1, "{ x : 1}"));
    });
}

TEST_F(UnstructuredLoggingTest, ManyArgs) {
    std::string format_str = "{}{}{}{}{}{}{}{}{}{}{}";
    logd(format_str, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);  // NOLINT
    validate([&format_str](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(),
                      fmt::format(fmt::runtime(format_str), 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11));
    });
}

TEST_F(UnstructuredLoggingTest, UserToString) {
    TypeWithoutBSON arg(1.0, 2.0);
    logd("{}", arg);  // NOLINT
    validate([&arg](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), arg.toString());
    });
}

TEST_F(UnstructuredLoggingTest, UserToBSON) {
    struct TypeWithOnlyBSON {
        BSONObj toBSON() const {
            return BSONObjBuilder{}.append("x", 1).append("y", 2).obj();
        }
    };
    TypeWithOnlyBSON arg;
    logd("{}", arg);  // NOLINT
    validate([&arg](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), arg.toBSON().toString());
    });
}

TEST_F(UnstructuredLoggingTest, UserBothStringAndBSON) {
    TypeWithBSON arg(1.0, 2.0);
    logd("{}", arg);  // NOLINT
    validate([&arg](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), arg.toString());
    });
}

TEST_F(UnstructuredLoggingTest, VectorBSON) {
    std::vector<BSONObj> vectorBSON = {BSON("str1" << "str2"), BSON("str3" << "str4")};
    logd("{}", vectorBSON);  // NOLINT
    validate([](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(),
                      "({\"str1\":\"str2\"}, {\"str3\":\"str4\"})");
    });
}

TEST_F(UnstructuredLoggingTest, MapBSON) {
    std::map<std::string, BSONObj> mapBSON = {{"key1", BSON("str1" << "str2")},
                                              {"key2", BSON("str3" << "str4")}};
    logd("{}", mapBSON);  // NOLINT
    validate([](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(),
                      "(key1: {\"str1\":\"str2\"}, key2: {\"str3\":\"str4\"})");
    });
}


// Tests for the `LogSeverity::ProdOnly()` level. In a testing environment, this should behave like
// a debug-1 log but in a production environment like a default (debug-0) log.
class ProdOnlySeverityTest : public LogV2Test {
public:
    using LogV2Test::LogV2Test;

    // Returns the most severe severity for which the log appears. The log will appear at all
    // severities below this one as well.
    boost::optional<LogSeverity> findFirstSeverityLogged(bool options) {
        LogSeverity severity = LogSeverity::Log();

        while (severity.toInt() < 6) {
            // The logLevel server parameter accepts an integer 0 ('LogSeverity::Log()') to 5
            // ('LogSeverity::Debug(5)') as it's argument.
            RAIIServerParameterControllerForTest logVerbosityController{"logLevel",
                                                                        severity.toInt()};
            if (options) {
                LOGV2_PROD_ONLY_OPTIONS(9757701, {LogTag::kNone}, "test");
            } else {
                LOGV2_PROD_ONLY(9757700, "test");
            }
            if (lines->size() != 0) {
                return severity;
            }
            severity = severity.lessSevere();
        }
        return boost::none;
    }

    std::unique_ptr<LineCapture> lines = makeLineCapture(PlainFormatter());
};

// Tests that '_suppressProdOnly' is configured with the enableTestCommands parameter.
TEST_F(ProdOnlySeverityTest, SuppressProdOnlyTrueWhenEnableTestCommandsTrue) {
    RAIIServerParameterControllerForTest enableTestCommandsController{"enableTestCommands", false};
    ASSERT_EQUALS(LogSeverity::getSuppressProdOnly(), false);
}

// Tests that '_suppressProdOnly' is configured with the enableTestCommands parameter.
TEST_F(ProdOnlySeverityTest, SuppressProdOnlyFalseWhenEnableTestCommandsFalse) {
    RAIIServerParameterControllerForTest enableTestCommandsController{"enableTestCommands", true};
    ASSERT_EQUALS(LogSeverity::getSuppressProdOnly(), true);
}

// Tests that LogSeverity::ProdOnly() returns correct severity in production.
TEST_F(ProdOnlySeverityTest, ProdOnlySeverityIsZeroInProd) {
    LogSeverity::suppressProdOnly_forTest(false);
    ASSERT_EQUALS(LogSeverity::ProdOnly(), LogSeverity::Log());
}

// Tests that LogSeverty::ProdOnly() returns correct severity in testing.
TEST_F(ProdOnlySeverityTest, ProdOnlySeveritIsOneInTest) {
    LogSeverity::suppressProdOnly_forTest(true);
    ASSERT_EQUALS(LogSeverity::ProdOnly(), LogSeverity::Debug(1));
}

// In production we expect to first see the message at the default level 0 for 'LOGV2_PROD_ONLY'.
TEST_F(ProdOnlySeverityTest, LogAtDefaultInProd) {
    LogSeverity::suppressProdOnly_forTest(false);
    ASSERT_EQUALS(findFirstSeverityLogged(false), LogSeverity::Log());
}

// In testing we expect to first see the message at the debug -1 for 'LOGV2_PROD_ONLY'.
TEST_F(ProdOnlySeverityTest, LogAtDebug1InTest) {
    LogSeverity::suppressProdOnly_forTest(true);
    ASSERT_EQUALS(findFirstSeverityLogged(false), LogSeverity::Debug(1));
}

// In production we expect to first see the message at the default level 0 for
// 'LOGV2_PROD_ONLY_OPTIONS'.
TEST_F(ProdOnlySeverityTest, LogOptionsAtDefaultInProd) {
    LogSeverity::suppressProdOnly_forTest(false);
    ASSERT_EQUALS(findFirstSeverityLogged(true), LogSeverity::Log());
}

// In testing we expect to first see the message at the debug-1 for 'LOGV2_PROD_ONLY_OPTIONS'.
TEST_F(ProdOnlySeverityTest, LogOptionsAtDebug1InTest) {
    LogSeverity::suppressProdOnly_forTest(true);
    ASSERT_EQUALS(findFirstSeverityLogged(true), LogSeverity::Debug(1));
}

}  // namespace
}  // namespace mongo::logv2
