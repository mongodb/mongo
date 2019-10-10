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
#include <string>
#include <vector>

#include "mongo/bson/json.h"
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/formatter_base.h"
#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/ramlog_sink.h"
#include "mongo/logv2/text_formatter.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/temp_dir.h"

#include <boost/log/attributes/constant.hpp>

namespace {
struct TypeWithCustomFormatting {
    TypeWithCustomFormatting() {}
    TypeWithCustomFormatting(double x, double y) : _x(x), _y(y) {}

    double _x{0.0};
    double _y{0.0};

    std::string toString() const {
        return fmt::format("(x: {}, y: {})", _x, _y);
    }

    std::string toJson() const {
        return fmt::format("{{\"x\": {}, \"y\": {}}}", _x, _y);
    }
};
}  // namespace


namespace fmt {
template <>
struct formatter<TypeWithCustomFormatting> : public mongo::logv2::FormatterBase {
    template <typename FormatContext>
    auto format(const TypeWithCustomFormatting& obj, FormatContext& ctx) {
        switch (output_format()) {
            case OutputFormat::kJson:
                return format_to(ctx.out(), "{}", obj.toJson());

            case OutputFormat::kBson:
                return format_to(ctx.out(), "{}", "bson impl here");

            case OutputFormat::kText:
            default:
                return format_to(ctx.out(), "{}", obj.toString());
        }
    }
};
}  // namespace fmt

namespace mongo {
namespace logv2 {
namespace {
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

class PlainFormatter {
public:
    static bool binary() {
        return false;
    };

    void operator()(boost::log::record_view const& rec, boost::log::formatting_ostream& strm) {
        StringData message = boost::log::extract<StringData>(attributes::message(), rec).get();
        const auto& attrs =
            boost::log::extract<AttributeArgumentSet>(attributes::attributes(), rec).get();

        strm << fmt::internal::vformat(to_string_view(message), attrs._values);
    }
};

class LogDuringInitTester {
public:
    LogDuringInitTester() {
        std::vector<std::string> lines;
        auto sink = LogTestBackend::create(lines);
        sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain()));
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
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    LOGV2("test");
    ASSERT(lines.back() == "test");

    LOGV2("test {}", "name"_attr = 1);
    ASSERT(lines.back() == "test 1");

    LOGV2("test {:d}", "name"_attr = 2);
    ASSERT(lines.back() == "test 2");

    LOGV2("test {}", "name"_attr = "char*");
    ASSERT(lines.back() == "test char*");

    LOGV2("test {}", "name"_attr = std::string("std::string"));
    ASSERT(lines.back() == "test std::string");

    LOGV2("test {}", "name"_attr = "StringData"_sd);
    ASSERT(lines.back() == "test StringData");

    LOGV2_OPTIONS({LogTag::kStartupWarnings}, "test");
    ASSERT(lines.back() == "test");

    TypeWithCustomFormatting t(1.0, 2.0);
    LOGV2("{} custom formatting", "name"_attr = t);
    ASSERT(lines.back() == t.toString() + " custom formatting");

    LOGV2("{:j} custom formatting, force json", "name"_attr = t);
    ASSERT(lines.back() == t.toJson() + " custom formatting, force json");
}

TEST_F(LogTestV2, TextFormat) {
    std::vector<std::string> lines;
    auto sink = LogTestBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain()));
    sink->set_formatter(TextFormatter());
    attach(sink);

    LOGV2_OPTIONS({LogTag::kNone}, "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") == std::string::npos);

    LOGV2_OPTIONS({LogTag::kStartupWarnings}, "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") != std::string::npos);

    LOGV2_OPTIONS({static_cast<LogTag::Value>(LogTag::kStartupWarnings | LogTag::kJavascript)},
                  "warning");
    ASSERT(lines.back().rfind("** WARNING: warning") != std::string::npos);

    TypeWithCustomFormatting t(1.0, 2.0);
    LOGV2("{} custom formatting", "name"_attr = t);
    ASSERT(lines.back().rfind(t.toString() + " custom formatting") != std::string::npos);
}

TEST_F(LogTestV2, JSONFormat) {
    std::vector<std::string> lines;
    auto sink = LogTestBackend::create(lines);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain()));
    sink->set_formatter(JsonFormatter());
    attach(sink);

    BSONObj log;

    LOGV2("test");
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("t"_sd).String() == dateToISOStringUTC(Date_t::lastNowForTest()));
    ASSERT(log.getField("s"_sd).String() == LogSeverity::Info().toStringDataCompact());
    ASSERT(log.getField("c"_sd).String() ==
           LogComponent(MONGO_LOGV2_DEFAULT_COMPONENT).getNameForLog());
    ASSERT(log.getField("ctx"_sd).String() == getThreadName());
    ASSERT(!log.hasField("id"_sd));
    ASSERT(log.getField("msg"_sd).String() == "test");
    ASSERT(!log.hasField("attr"_sd));
    ASSERT(!log.hasField("tags"_sd));

    LOGV2("test {}", "name"_attr = 1);
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("msg"_sd).String() == "test {name}");
    ASSERT(log.getField("attr"_sd).Obj().nFields() == 1);
    ASSERT(log.getField("attr"_sd).Obj().getField("name").Int() == 1);

    LOGV2("test {:d}", "name"_attr = 2);
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("msg"_sd).String() == "test {name:d}");
    ASSERT(log.getField("attr"_sd).Obj().nFields() == 1);
    ASSERT(log.getField("attr"_sd).Obj().getField("name").Int() == 2);

    LOGV2_OPTIONS({LogTag::kStartupWarnings}, "warning");
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("msg"_sd).String() == "warning");
    ASSERT(log.getField("tags"_sd).Array().front().woCompare(
               mongo::fromjson(LogTag(LogTag::kStartupWarnings).toJSONArray())[0]) == 0);

    LOGV2_OPTIONS({LogComponent::kControl}, "different component");
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("c"_sd).String() == LogComponent(LogComponent::kControl).getNameForLog());
    ASSERT(log.getField("msg"_sd).String() == "different component");

    TypeWithCustomFormatting t(1.0, 2.0);
    LOGV2("{} custom formatting", "name"_attr = t);
    log = mongo::fromjson(lines.back());
    ASSERT(log.getField("msg"_sd).String() == "{name} custom formatting");
    ASSERT(log.getField("attr"_sd).Obj().nFields() == 1);
    ASSERT(log.getField("attr"_sd).Obj().getField("name").Obj().woCompare(
               mongo::fromjson(t.toJson())) == 0);
}

TEST_F(LogTestV2, Threads) {
    std::vector<std::string> linesPlain;
    auto plainSink = LogTestBackend::create(linesPlain);
    plainSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain()));
    plainSink->set_formatter(PlainFormatter());
    attach(plainSink);

    std::vector<std::string> linesText;
    auto textSink = LogTestBackend::create(linesText);
    textSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain()));
    textSink->set_formatter(TextFormatter());
    attach(textSink);

    std::vector<std::string> linesJson;
    auto jsonSink = LogTestBackend::create(linesJson);
    jsonSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain()));
    jsonSink->set_formatter(JsonFormatter());
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

    auto sink = RamLogSink::create(ramlog);
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain()));
    sink->set_formatter(PlainFormatter());
    attach(sink);

    std::vector<std::string> lines;
    auto testSink = LogTestBackend::create(lines);
    testSink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain()));
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
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain()));
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
    other_sink->set_filter(ComponentSettingsFilter(other_domain));
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
    sink->set_filter(ComponentSettingsFilter(LogManager::global().getGlobalDomain()));
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
