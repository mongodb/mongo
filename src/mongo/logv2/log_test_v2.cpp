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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/logv2/log_test_v2.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/logv2/bson_formatter.h"
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/formatter_base.h"
#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/logv2/ramlog_sink.h"
#include "mongo/logv2/text_formatter.h"
#include "mongo/platform/decimal128.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/uuid.h"

#include <boost/log/attributes/constant.hpp>
#include <boost/optional.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace mongo {
namespace logv2 {
namespace {

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
        fmt::format_to(buffer, "(x: {}, y: {})", _x, _y);
    }
};

struct TypeWithBothStringFormatters {
    TypeWithBothStringFormatters() {}

    std::string toString() const {
        return fmt::format("toString");
    }

    void serialize(fmt::memory_buffer& buffer) const {
        fmt::format_to(buffer, "serialize");
    }
};

struct TypeWithBSON : public TypeWithoutBSON {
    using TypeWithoutBSON::TypeWithoutBSON;

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        builder.append("x"_sd, _x);
        builder.append("y"_sd, _y);
        return builder.obj();
    }
};

struct TypeWithBSONSerialize : public TypeWithoutBSON {
    using TypeWithoutBSON::TypeWithoutBSON;

    void serialize(BSONObjBuilder* builder) const {
        builder->append("x"_sd, _x);
        builder->append("y"_sd, _y);
        builder->append("type"_sd, "serialize"_sd);
    }
};

struct TypeWithBothBSONFormatters : public TypeWithBSON {
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

class LogTestBackend
    : public boost::log::sinks::
          basic_formatted_sink_backend<char, boost::log::sinks::synchronized_feeding> {
public:
    LogTestBackend(std::vector<std::string>& lines) : _logLines(lines) {}

    static boost::shared_ptr<boost::log::sinks::synchronous_sink<LogTestBackend>> create(
        std::vector<std::string>& lines) {
        auto backend = boost::make_shared<LogTestBackend>(lines);
        return boost::make_shared<boost::log::sinks::synchronous_sink<LogTestBackend>>(
            std::move(backend));
    }

    void consume(boost::log::record_view const& rec, string_type const& formatted_string) {
        _logLines.push_back(formatted_string);
    }

private:
    std::vector<std::string>& _logLines;
};

class LogDuringInitTester {
public:
    LogDuringInitTester() {
        std::vector<std::string> lines;
        auto sink = LogTestBackend::create(lines);
        sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
        sink->set_formatter(PlainFormatter());
        boost::log::core::get()->add_sink(sink);

        LOGV2("log during init");
        ASSERT(lines.back() == "log during init");

        boost::log::core::get()->remove_sink(sink);
    }
};

LogDuringInitTester logDuringInit;

TEST_F(LogTestV2, Basic) {
    std::vector<std::string> lines;
    auto sink = LogTestBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    BSONObjBuilder builder;
    fmt::memory_buffer buffer;

    LOGV2("test");
    ASSERT_EQUALS(lines.back(), "test");

    LOGV2_DEBUG(-2, "test debug");
    ASSERT_EQUALS(lines.back(), "test debug");

    LOGV2("test {}", "name"_attr = 1);
    ASSERT_EQUALS(lines.back(), "test 1");

    LOGV2("test {:d}", "name"_attr = 2);
    ASSERT_EQUALS(lines.back(), "test 2");

    LOGV2("test {}", "name"_attr = "char*");
    ASSERT_EQUALS(lines.back(), "test char*");

    LOGV2("test {}", "name"_attr = std::string("std::string"));
    ASSERT_EQUALS(lines.back(), "test std::string");

    LOGV2("test {}", "name"_attr = "StringData"_sd);
    ASSERT_EQUALS(lines.back(), "test StringData");

    LOGV2_OPTIONS({LogTag::kStartupWarnings}, "test");
    ASSERT_EQUALS(lines.back(), "test");

    TypeWithBSON t(1.0, 2.0);
    LOGV2("{} custom formatting", "name"_attr = t);
    ASSERT_EQUALS(lines.back(), t.toString() + " custom formatting");

    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2("{} custom formatting, no bson", "name"_attr = t2);
    ASSERT_EQUALS(lines.back(), t.toString() + " custom formatting, no bson");

    TypeWithOnlyStringSerialize t3(1.0, 2.0);
    LOGV2("{}", "name"_attr = t3);
    buffer.clear();
    t3.serialize(buffer);
    ASSERT_EQUALS(lines.back(), fmt::to_string(buffer));

    // Serialize should be preferred when both are available
    TypeWithBothStringFormatters t4;
    LOGV2("{}", "name"_attr = t4);
    buffer.clear();
    t4.serialize(buffer);
    ASSERT_EQUALS(lines.back(), fmt::to_string(buffer));
}

TEST_F(LogTestV2, Types) {
    using namespace constants;

    std::vector<std::string> text;
    auto text_sink = LogTestBackend::create(text);
    text_sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    text_sink->set_formatter(PlainFormatter());
    attach(text_sink);

    std::vector<std::string> json;
    auto json_sink = LogTestBackend::create(json);
    json_sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    json_sink->set_formatter(JSONFormatter());
    attach(json_sink);

    std::vector<std::string> bson;
    auto bson_sink = LogTestBackend::create(bson);
    bson_sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    bson_sink->set_formatter(BSONFormatter());
    attach(bson_sink);

    // The JSON formatter should make the types round-trippable without data loss
    auto validateJSON = [&](auto expected) {
        namespace pt = boost::property_tree;

        std::istringstream json_stream(json.back());
        pt::ptree ptree;
        pt::json_parser::read_json(json_stream, ptree);
        ASSERT_EQUALS(ptree.get<decltype(expected)>(std::string(kAttributesFieldName) + ".name"),
                      expected);
    };

    auto lastBSONElement = [&]() {
        return BSONObj(bson.back().data()).getField(kAttributesFieldName).Obj().getField("name"_sd);
    };

    auto testIntegral = [&](auto dummy) {
        using T = decltype(dummy);

        auto test = [&](auto value) {
            text.clear();
            LOGV2("{}", "name"_attr = value);
            ASSERT_EQUALS(text.back(), fmt::format("{}", value));
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
            text.clear();
            LOGV2("{}", "name"_attr = value);
            // Floats are formatted as double
            ASSERT_EQUALS(text.back(), fmt::format("{}", static_cast<double>(value)));
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
    LOGV2("bool {}", "name"_attr = b);
    ASSERT_EQUALS(text.back(), "bool true");
    validateJSON(b);
    ASSERT(lastBSONElement().Bool() == b);

    char c = 1;
    LOGV2("char {}", "name"_attr = c);
    ASSERT_EQUALS(text.back(), "char 1");
    validateJSON(static_cast<uint8_t>(
        c));  // cast, boost property_tree will try and parse as ascii otherwise
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

    // string types
    const char* c_str = "a c string";
    LOGV2("c string {}", "name"_attr = c_str);
    ASSERT_EQUALS(text.back(), "c string a c string");
    validateJSON(std::string(c_str));
    ASSERT_EQUALS(lastBSONElement().String(), c_str);

    char* c_str2 = const_cast<char*>("non-const");
    LOGV2("c string {}", "name"_attr = c_str2);
    ASSERT_EQUALS(text.back(), "c string non-const");
    validateJSON(std::string(c_str2));
    ASSERT_EQUALS(lastBSONElement().String(), c_str2);

    std::string str = "a std::string";
    LOGV2("std::string {}", "name"_attr = str);
    ASSERT_EQUALS(text.back(), "std::string a std::string");
    validateJSON(str);
    ASSERT_EQUALS(lastBSONElement().String(), str);

    StringData str_data = "a StringData"_sd;
    LOGV2("StringData {}", "name"_attr = str_data);
    ASSERT_EQUALS(text.back(), "StringData a StringData");
    validateJSON(str_data.toString());
    ASSERT_EQUALS(lastBSONElement().String(), str_data);

    // BSONObj
    BSONObjBuilder builder;
    builder.append("int32"_sd, 1);
    builder.append("int64"_sd, std::numeric_limits<int64_t>::max());
    builder.append("double"_sd, 1.0);
    builder.append("str"_sd, str_data);
    BSONObj bsonObj = builder.obj();
    LOGV2("bson {}", "name"_attr = bsonObj);
    ASSERT(text.back() ==
           std::string("bson ") + bsonObj.jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0));
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Obj()
               .woCompare(bsonObj) == 0);
    ASSERT(lastBSONElement().Obj().woCompare(bsonObj) == 0);

    // BSONArray
    BSONArrayBuilder arrBuilder;
    arrBuilder.append("first"_sd);
    arrBuilder.append("second"_sd);
    arrBuilder.append("third"_sd);
    BSONArray bsonArr = arrBuilder.arr();
    LOGV2("{}", "name"_attr = bsonArr);
    ASSERT_EQUALS(text.back(),
                  bsonArr.jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0, 0, true));
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Obj()
               .woCompare(bsonArr) == 0);
    ASSERT(lastBSONElement().Obj().woCompare(bsonArr) == 0);

    // BSONElement
    LOGV2("bson element {}", "name"_attr = bsonObj.getField("int32"_sd));
    ASSERT(text.back() == std::string("bson element ") + bsonObj.getField("int32"_sd).toString());
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name"_sd)
               .Obj()
               .getField("int32"_sd)
               .Int() == bsonObj.getField("int32"_sd).Int());
    ASSERT(lastBSONElement().Obj().getField("int32"_sd).Int() ==
           bsonObj.getField("int32"_sd).Int());

    // Date_t
    Date_t date = Date_t::now();
    LOGV2("Date_t {}", "name"_attr = date);
    ASSERT_EQUALS(text.back(), std::string("Date_t ") + date.toString());
    ASSERT_EQUALS(
        mongo::fromjson(json.back()).getField(kAttributesFieldName).Obj().getField("name").Date(),
        date);
    ASSERT_EQUALS(lastBSONElement().Date(), date);

    // Decimal128
    LOGV2("Decimal128 {}", "name"_attr = Decimal128::kPi);
    ASSERT_EQUALS(text.back(), std::string("Decimal128 ") + Decimal128::kPi.toString());
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Decimal()
               .isEqual(Decimal128::kPi));
    ASSERT(lastBSONElement().Decimal().isEqual(Decimal128::kPi));

    // OID
    OID oid = OID::gen();
    LOGV2("OID {}", "name"_attr = oid);
    ASSERT_EQUALS(text.back(), std::string("OID ") + oid.toString());
    ASSERT_EQUALS(
        mongo::fromjson(json.back()).getField(kAttributesFieldName).Obj().getField("name").OID(),
        oid);
    ASSERT_EQUALS(lastBSONElement().OID(), oid);

    // Timestamp
    Timestamp ts = Timestamp::max();
    LOGV2("Timestamp {}", "name"_attr = ts);
    ASSERT_EQUALS(text.back(), std::string("Timestamp ") + ts.toString());
    ASSERT_EQUALS(mongo::fromjson(json.back())
                      .getField(kAttributesFieldName)
                      .Obj()
                      .getField("name")
                      .timestamp(),
                  ts);
    ASSERT_EQUALS(lastBSONElement().timestamp(), ts);

    // UUID
    UUID uuid = UUID::gen();
    LOGV2("UUID {}", "name"_attr = uuid);
    ASSERT_EQUALS(text.back(), std::string("UUID ") + uuid.toString());
    ASSERT_EQUALS(UUID::parse(mongo::fromjson(json.back())
                                  .getField(kAttributesFieldName)
                                  .Obj()
                                  .getField("name")
                                  .Obj()),
                  uuid);
    ASSERT_EQUALS(UUID::parse(lastBSONElement().Obj()), uuid);

    // boost::optional
    LOGV2("boost::optional empty {}", "name"_attr = boost::optional<bool>());
    ASSERT_EQUALS(text.back(),
                  std::string("boost::optional empty ") +
                      constants::kNullOptionalString.toString());
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .isNull());
    ASSERT(lastBSONElement().isNull());

    LOGV2("boost::optional<bool> {}", "name"_attr = boost::optional<bool>(true));
    ASSERT_EQUALS(text.back(), std::string("boost::optional<bool> true"));
    ASSERT_EQUALS(
        mongo::fromjson(json.back()).getField(kAttributesFieldName).Obj().getField("name").Bool(),
        true);
    ASSERT_EQUALS(lastBSONElement().Bool(), true);

    LOGV2("boost::optional<boost::optional<bool>> {}",
          "name"_attr = boost::optional<boost::optional<bool>>(boost::optional<bool>(true)));
    ASSERT_EQUALS(text.back(), std::string("boost::optional<boost::optional<bool>> true"));
    ASSERT_EQUALS(
        mongo::fromjson(json.back()).getField(kAttributesFieldName).Obj().getField("name").Bool(),
        true);
    ASSERT_EQUALS(lastBSONElement().Bool(), true);

    TypeWithBSON withBSON(1.0, 2.0);
    LOGV2("boost::optional<TypeWithBSON> {}",
          "name"_attr = boost::optional<TypeWithBSON>(withBSON));
    ASSERT_EQUALS(text.back(), std::string("boost::optional<TypeWithBSON> ") + withBSON.toString());
    ASSERT(mongo::fromjson(json.back())
               .getField(kAttributesFieldName)
               .Obj()
               .getField("name")
               .Obj()
               .woCompare(withBSON.toBSON()) == 0);
    ASSERT(lastBSONElement().Obj().woCompare(withBSON.toBSON()) == 0);

    TypeWithoutBSON withoutBSON(1.0, 2.0);
    LOGV2("boost::optional<TypeWithBSON> {}",
          "name"_attr = boost::optional<TypeWithoutBSON>(withoutBSON));
    ASSERT_EQUALS(text.back(),
                  std::string("boost::optional<TypeWithBSON> ") + withoutBSON.toString());
    ASSERT_EQUALS(
        mongo::fromjson(json.back()).getField(kAttributesFieldName).Obj().getField("name").String(),
        withoutBSON.toString());
    ASSERT_EQUALS(lastBSONElement().String(), withoutBSON.toString());

    // Duration
    Milliseconds ms{12345};
    LOGV2("Duration {}", "name"_attr = ms);
    ASSERT_EQUALS(text.back(), std::string("Duration ") + ms.toString());
    ASSERT_EQUALS(mongo::fromjson(json.back())
                      .getField(kAttributesFieldName)
                      .Obj()
                      .getField("name")
                      .Obj()
                      .woCompare(ms.toBSON()),
                  0);
    ASSERT(lastBSONElement().Obj().woCompare(ms.toBSON()) == 0);
}

TEST_F(LogTestV2, TextFormat) {
    std::vector<std::string> lines;
    auto sink = LogTestBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(TextFormatter());
    attach(sink);

    LOGV2_OPTIONS({LogTag::kNone}, "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") == std::string::npos);

    LOGV2_OPTIONS({LogTag::kStartupWarnings}, "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") != std::string::npos);

    LOGV2_OPTIONS({static_cast<LogTag::Value>(LogTag::kStartupWarnings | LogTag::kPlainShell)},
                  "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") != std::string::npos);

    TypeWithBSON t(1.0, 2.0);
    LOGV2("{} custom formatting", "name"_attr = t);
    ASSERT(lines.back().rfind(t.toString() + " custom formatting") != std::string::npos);

    LOGV2("{} bson", "name"_attr = t.toBSON());
    ASSERT(lines.back().rfind(t.toBSON().jsonString(JsonStringFormat::ExtendedRelaxedV2_0_0) +
                              " bson") != std::string::npos);

    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2("{} custom formatting, no bson", "name"_attr = t2);
    ASSERT(lines.back().rfind(t.toString() + " custom formatting, no bson") != std::string::npos);
}

TEST_F(LogTestV2, JsonBsonFormat) {
    using namespace constants;

    std::vector<std::string> lines;
    auto sink = LogTestBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(JSONFormatter());
    attach(sink);

    std::vector<std::string> linesBson;
    auto sinkBson = LogTestBackend::create(linesBson);
    sinkBson->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
    sinkBson->set_formatter(BSONFormatter());
    attach(sinkBson);

    BSONObj log;

    LOGV2("test");
    auto validateRoot = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kTimestampFieldName).Date(), Date_t::lastNowForTest());
        ASSERT_EQUALS(obj.getField(kSeverityFieldName).String(),
                      LogSeverity::Info().toStringDataCompact());
        ASSERT_EQUALS(obj.getField(kComponentFieldName).String(),
                      LogComponent(MONGO_LOGV2_DEFAULT_COMPONENT).getNameForLog());
        ASSERT(obj.getField(kContextFieldName).String() == getThreadName());
        ASSERT(!obj.hasField(kStableIdFieldName));
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test");
        ASSERT(!obj.hasField(kAttributesFieldName));
        ASSERT(!obj.hasField(kTagsFieldName));
    };
    validateRoot(mongo::fromjson(lines.back()));
    validateRoot(BSONObj(linesBson.back().data()));


    LOGV2("test {}", "name"_attr = 1);
    auto validateAttr = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test {name}");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").Int(), 1);
    };
    validateAttr(mongo::fromjson(lines.back()));
    validateAttr(BSONObj(linesBson.back().data()));


    LOGV2("test {:d}", "name"_attr = 2);
    auto validateMsgReconstruction = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test {name:d}");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").Int(), 2);
    };
    validateMsgReconstruction(mongo::fromjson(lines.back()));
    validateMsgReconstruction(BSONObj(linesBson.back().data()));

    LOGV2("test {: <4}", "name"_attr = 2);
    auto validateMsgReconstruction2 = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "test {name: <4}");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").Int(), 2);
    };
    validateMsgReconstruction2(mongo::fromjson(lines.back()));
    validateMsgReconstruction2(BSONObj(linesBson.back().data()));


    LOGV2_OPTIONS({LogTag::kStartupWarnings}, "warning");
    auto validateTags = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "warning");
        ASSERT_EQUALS(
            obj.getField("tags"_sd).Obj().woCompare(LogTag(LogTag::kStartupWarnings).toBSONArray()),
            0);
    };
    validateTags(mongo::fromjson(lines.back()));
    validateTags(BSONObj(linesBson.back().data()));

    LOGV2_OPTIONS({LogComponent::kControl}, "different component");
    auto validateComponent = [](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField("c"_sd).String(),
                      LogComponent(LogComponent::kControl).getNameForLog());
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "different component");
    };
    validateComponent(mongo::fromjson(lines.back()));
    validateComponent(BSONObj(linesBson.back().data()));


    TypeWithBSON t(1.0, 2.0);
    LOGV2("{} custom formatting", "name"_attr = t);
    auto validateCustomAttr = [&t](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "{name} custom formatting");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT(
            obj.getField(kAttributesFieldName).Obj().getField("name").Obj().woCompare(t.toBSON()) ==
            0);
    };
    validateCustomAttr(mongo::fromjson(lines.back()));
    validateCustomAttr(BSONObj(linesBson.back().data()));


    LOGV2("{} bson", "name"_attr = t.toBSON());
    auto validateBsonAttr = [&t](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "{name} bson");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT(
            obj.getField(kAttributesFieldName).Obj().getField("name").Obj().woCompare(t.toBSON()) ==
            0);
    };
    validateBsonAttr(mongo::fromjson(lines.back()));
    validateBsonAttr(BSONObj(linesBson.back().data()));


    TypeWithoutBSON t2(1.0, 2.0);
    LOGV2("{} custom formatting", "name"_attr = t2);
    auto validateCustomAttrWithoutBSON = [&t2](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kMessageFieldName).String(), "{name} custom formatting");
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().nFields(), 1);
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").String(),
                      t2.toString());
    };
    validateCustomAttrWithoutBSON(mongo::fromjson(lines.back()));
    validateCustomAttrWithoutBSON(BSONObj(linesBson.back().data()));

    TypeWithBSONSerialize t3(1.0, 2.0);
    LOGV2("{}", "name"_attr = t3);
    auto validateCustomAttrBSONSerialize = [&t3](const BSONObj& obj) {
        BSONObjBuilder builder;
        t3.serialize(&builder);
        ASSERT(obj.getField(kAttributesFieldName)
                   .Obj()
                   .getField("name")
                   .Obj()
                   .woCompare(builder.done()) == 0);
    };
    validateCustomAttrBSONSerialize(mongo::fromjson(lines.back()));
    validateCustomAttrBSONSerialize(BSONObj(linesBson.back().data()));


    TypeWithBothBSONFormatters t4(1.0, 2.0);
    LOGV2("{}", "name"_attr = t4);
    auto validateCustomAttrBSONBothFormatters = [&t4](const BSONObj& obj) {
        BSONObjBuilder builder;
        t4.serialize(&builder);
        ASSERT(obj.getField(kAttributesFieldName)
                   .Obj()
                   .getField("name")
                   .Obj()
                   .woCompare(builder.done()) == 0);
    };
    validateCustomAttrBSONBothFormatters(mongo::fromjson(lines.back()));
    validateCustomAttrBSONBothFormatters(BSONObj(linesBson.back().data()));

    TypeWithBSONArray t5;
    LOGV2("{}", "name"_attr = t5);
    auto validateCustomAttrBSONArray = [&t5](const BSONObj& obj) {
        ASSERT_EQUALS(obj.getField(kAttributesFieldName).Obj().getField("name").type(),
                      BSONType::Array);
        ASSERT(obj.getField(kAttributesFieldName)
                   .Obj()
                   .getField("name")
                   .Obj()
                   .woCompare(t5.toBSONArray()) == 0);
    };
    validateCustomAttrBSONArray(mongo::fromjson(lines.back()));
    validateCustomAttrBSONArray(BSONObj(linesBson.back().data()));
}

TEST_F(LogTestV2, Unicode) {
    std::vector<std::string> lines;
    auto sink = LogTestBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    std::pair<StringData, StringData> strs[] = {
        // Single byte characters that needs to be escaped
        {"\a\b\f\n\r\t\v\\\0\x7f\x1b"_sd, "\\a\\b\\f\\n\\r\\t\\v\\\\\\0\\x7f\\e"_sd},
        // multi byte characters that needs to be escaped (unicode control characters)
        {"\u0080\u009f"_sd, "\\xc2\\x80\\xc2\\x9f"_sd},
        // Valid 2 Octet sequence, LATIN SMALL LETTER N WITH TILDE
        {"\u00f1"_sd, "\u00f1"_sd},
        // Invalid 2 Octet Sequence, result is escaped
        {"\xc3\x28"_sd, "\\xc3\x28"_sd},
        // Invalid Sequence Identifier, result is escaped
        {"\xa0\xa1"_sd, "\\xa0\\xa1"_sd},
        // Valid 3 Octet sequence, RUNIC LETTER TIWAZ TIR TYR T
        {"\u16cf"_sd, "\u16cf"_sd},
        // Invalid 3 Octet Sequence (in 2nd Octet), result is escaped
        {"\xe2\x28\xa1"_sd, "\\xe2\x28\\xa1"_sd},
        // Invalid 3 Octet Sequence (in 3rd Octet), result is escaped
        {"\xe2\x82\x28"_sd, "\\xe2\\x82\x28"_sd},
        // Valid 4 Octet sequence, GOTHIC LETTER MANNA
        {"\U0001033c"_sd, "\U0001033c"_sd},
        // Invalid 4 Octet Sequence (in 2nd Octet), result is escaped
        {"\xf0\x28\x8c\xbc"_sd, "\\xf0\x28\\x8c\\xbc"_sd},
        // Invalid 4 Octet Sequence (in 3rd Octet), result is escaped
        {"\xf0\x90\x28\xbc"_sd, "\\xf0\\x90\x28\\xbc"_sd},
        // Invalid 4 Octet Sequence (in 4th Octet), result is escaped
        {"\xf0\x28\x8c\x28"_sd, "\\xf0\x28\\x8c\x28"_sd},
        // Valid 5 Octet Sequence (but not Unicode!), result is escaped
        {"\xf8\xa1\xa1\xa1\xa1"_sd, "\\xf8\\xa1\\xa1\\xa1\\xa1"_sd},
        // Valid 6 Octet Sequence (but not Unicode!), result is escaped
        {"\xfc\xa1\xa1\xa1\xa1\xa1"_sd, "\\xfc\\xa1\\xa1\\xa1\\xa1\\xa1"_sd},
        // Invalid 3 Octet sequence, buffer ends prematurely, result is escaped
        {"\xe2\x82"_sd, "\\xe2\\x82"_sd},
    };

    for (const auto& pair : strs) {
        LOGV2("{}", "name"_attr = pair.first);
        ASSERT_EQUALS(lines.back(), pair.second);
    }
}

TEST_F(LogTestV2, Threads) {
    std::vector<std::string> linesPlain;
    auto plainSink = LogTestBackend::create(linesPlain);
    plainSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                  LogManager::global().getGlobalSettings()));
    plainSink->set_formatter(PlainFormatter());
    attach(plainSink);

    std::vector<std::string> linesText;
    auto textSink = LogTestBackend::create(linesText);
    textSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
    textSink->set_formatter(TextFormatter());
    attach(textSink);

    std::vector<std::string> linesJson;
    auto jsonSink = LogTestBackend::create(linesJson);
    jsonSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
    jsonSink->set_formatter(JSONFormatter());
    attach(jsonSink);

    constexpr int kNumPerThread = 1000;
    std::vector<stdx::thread> threads;

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2("thread1");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2("thread2");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2("thread3");
    });

    threads.emplace_back([&]() {
        for (int i = 0; i < kNumPerThread; ++i)
            LOGV2("thread4");
    });

    for (auto&& thread : threads) {
        thread.join();
    }

    ASSERT(linesPlain.size() == threads.size() * kNumPerThread);
    ASSERT(linesText.size() == threads.size() * kNumPerThread);
    ASSERT(linesJson.size() == threads.size() * kNumPerThread);
}

TEST_F(LogTestV2, Ramlog) {
    RamLog* ramlog = RamLog::get("test_ramlog");

    auto sink = boost::make_shared<boost::log::sinks::unlocked_sink<RamLogSink>>(
        boost::make_shared<RamLogSink>(ramlog));
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    std::vector<std::string> lines;
    auto testSink = LogTestBackend::create(lines);
    testSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                                 LogManager::global().getGlobalSettings()));
    testSink->set_formatter(PlainFormatter());
    attach(testSink);

    auto verifyRamLog = [&]() {
        RamLog::LineIterator iter(ramlog);
        return std::all_of(lines.begin(), lines.end(), [&iter](const std::string& line) {
            return line == iter.next();
        });
    };

    LOGV2("test");
    ASSERT(verifyRamLog());
    LOGV2("test2");
    ASSERT(verifyRamLog());
}

TEST_F(LogTestV2, MultipleDomains) {
    std::vector<std::string> global_lines;
    auto sink = LogTestBackend::create(global_lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    // Example how a second domain can be created.
    class OtherDomainImpl : public LogDomain::Internal {
    public:
        OtherDomainImpl() {}

        LogSource& source() override {
            thread_local LogSource lg(this);
            return lg;
        }
    };

    LogDomain other_domain(std::make_unique<OtherDomainImpl>());
    std::vector<std::string> other_lines;
    auto other_sink = LogTestBackend::create(other_lines);
    other_sink->set_filter(
        ComponentSettingsFilter(other_domain, LogManager::global().getGlobalSettings()));
    other_sink->set_formatter(PlainFormatter());
    attach(other_sink);

    LOGV2_OPTIONS({&other_domain}, "test");
    ASSERT(global_lines.empty());
    ASSERT(other_lines.back() == "test");

    LOGV2("global domain log");
    ASSERT(global_lines.back() == "global domain log");
    ASSERT(other_lines.back() == "test");
}

TEST_F(LogTestV2, FileLogging) {
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

    auto sink = boost::make_shared<
        boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend>>(backend);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain(),
                                             LogManager::global().getGlobalSettings()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    auto readFile = [&](std::string const& filename) {
        std::vector<std::string> lines;
        std::ifstream file(filename);
        char line[1000] = {'\0'};

        while (true) {
            file.getline(line, sizeof(line), '\n');
            if (file.good()) {
                lines.emplace_back(line);
            } else
                break;
        }

        return lines;
    };

    LOGV2("test");
    ASSERT(readFile(file_name).back() == "test");

    LOGV2("test2");
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

}  // namespace
}  // namespace logv2
}  // namespace mongo
