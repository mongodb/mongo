// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/ramlog.h"

#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_capture_backend.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_domain_internal.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/logv2/ramlog_sink.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/enhanced_reporter.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <string>
#include <vector>

#include <boost/log/core/core.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/unlocked_frontend.hpp>
#include <boost/make_shared.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::logv2 {
namespace {

LogManager& mgr() {
    return LogManager::global();
}

std::vector<std::string> readRamLogLines(RamLog* ramlog) {
    std::vector<std::string> lines;
    RamLog::LineIterator iter(ramlog);
    lines.reserve(iter.len());
    while (iter.more()) {
        lines.emplace_back(iter.next());
    }
    return lines;
}

template <typename SinkPtr>
void applyDefaultFilterToSink(SinkPtr&& sink) {
    sink->set_filter(ComponentSettingsFilter(mgr().getGlobalDomain(), mgr().getGlobalSettings()));
}

class RamLogTest : public unittest::Test {
public:
    RamLogTest() {
        RamLog::setGlobalMaxLines(1024);
        RamLog::setGlobalMaxSizeBytes(1024 * 1024);
    }
};

// Framework to hook into LOGV2 macros and make sure RamLog sinks work
class RamLogSinkTest : public RamLogTest {
public:
    class Listener : public LogLineListener {
    public:
        explicit Listener(synchronized_value<std::vector<std::string>>& sv) : _sv(sv) {}
        void accept(const std::string& line) override {
            (**_sv).push_back(line);
        }

    private:
        synchronized_value<std::vector<std::string>>& _sv;
    };

    // Mostly lifted from logv2_test.cpp. Provide a sink to capture lines written to a RamLog, which
    // can then be compared against the lines held in the RamLog directly.
    class LineCapture {
    public:
        LineCapture() = delete;
        LineCapture(bool stripEol)
            : _sink{LogCaptureBackend::create(std::make_unique<Listener>(_syncedLines), stripEol)} {
        }
        auto lines() {
            return **_syncedLines;
        }
        auto& sink() {
            return _sink;
        }

    private:
        synchronized_value<std::vector<std::string>> _syncedLines;
        boost::shared_ptr<boost::log::sinks::unlocked_sink<LogCaptureBackend>> _sink;
    };

    RamLogSinkTest() {
        LogDomainGlobal::ConfigurationOptions config;
        config.makeDisabled();
        if (unittest::getGlobalEnhancedReporter()) {
            unittest::getGlobalEnhancedReporter()->disable();
        }
        ASSERT_OK(mgr().getGlobalDomainInternal().configure(config));
    }

    ~RamLogSinkTest() override {
        for (auto&& sink : _attachedSinks) {
            boost::log::core::get()->remove_sink(sink);
        }
        if (unittest::getGlobalEnhancedReporter()) {
            unittest::getGlobalEnhancedReporter()->enable();
        }
        ASSERT_OK(mgr().getGlobalDomainInternal().configure({}));
    }

    template <typename T>
    static auto wrapInUnlockedSink(boost::shared_ptr<T> sink) {
        return boost::make_shared<boost::log::sinks::unlocked_sink<T>>(std::move(sink));
    }

    void attachSink(boost::shared_ptr<boost::log::sinks::sink> sink) {
        boost::log::core::get()->add_sink(sink);
        _attachedSinks.push_back(sink);
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

TEST_F(RamLogSinkTest, BasicSink) {
    RamLog* ramlog = RamLog::get("TestRamlog");
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
    EXPECT_TRUE(verifyRamLog());
    LOGV2(20059, "test2");
    EXPECT_TRUE(verifyRamLog());
}

TEST_F(RamLogSinkTest, SinkAltMaxLinesMaxSize) {
    constexpr size_t alternativeMaxLines = 2048;
    constexpr size_t alternativeMaxSizeBytes = 2 * 1024 * 1024;
    RamLog::setGlobalMaxLines(alternativeMaxLines);
    RamLog::setGlobalMaxSizeBytes(alternativeMaxSizeBytes);
    RamLog* ramlog = RamLog::get("TestRamlogAlt2");
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
    EXPECT_TRUE(verifyRamLog());
    LOGV2(5816502, "test2");
    EXPECT_TRUE(verifyRamLog());
}

// Positive: Test that the ram log is properly circular
TEST_F(RamLogTest, CircularBuffer) {
    RamLog* ramlog = RamLog::get("TestRamlog2");

    std::vector<std::string> lines;

    constexpr size_t maxLines = 1024;
    constexpr size_t testLines = 5000;

    // Write enough lines to trigger wrapping
    for (size_t i = 0; i < testLines; ++i) {
        auto s = fmt::to_string(i);
        lines.push_back(s);
        ramlog->write(s);
    }

    lines.erase(lines.begin(), lines.begin() + (testLines - maxLines) + 1);

    // Verify we circled correctly through the buffer
    {
        RamLog::LineIterator iter(ramlog);
        EXPECT_EQ(iter.getTotalLinesWritten(), 5000UL);
        for (const auto& line : lines) {
            ASSERT_EQ(iter.next(), line);
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log is properly circular
TEST_F(RamLogTest, CircularBufferAltMaxLinesMaxSize) {
    constexpr size_t alternativeMaxLines = 10;
    constexpr size_t alternativeMaxSizeBytes = 2 * 1024 * 1024;
    RamLog::setGlobalMaxLines(alternativeMaxLines);
    RamLog::setGlobalMaxSizeBytes(alternativeMaxSizeBytes);
    RamLog* ramlog = RamLog::get("TestRamlog2Alt2");
    ASSERT_EQ(ramlog->getMaxLines(), alternativeMaxLines);
    ASSERT_EQ(ramlog->getMaxSizeBytes(), alternativeMaxSizeBytes);

    std::vector<std::string> lines;

    constexpr size_t maxLines = alternativeMaxLines;
    constexpr size_t testLines = 12;

    // Write enough lines to trigger wrapping
    for (size_t i = 0; i < testLines; ++i) {
        auto s = fmt::to_string(i);
        lines.push_back(s);
        ramlog->write(s);
    }

    lines.erase(lines.begin(), lines.begin() + (testLines - maxLines) + 1);

    // Verify we circled correctly through the buffer
    {
        RamLog::LineIterator iter(ramlog);
        EXPECT_EQ(iter.getTotalLinesWritten(), testLines);
        int n = 1;
        for (const auto& line : lines) {
            ASSERT_EQ(iter.next(), line) << "\n\n\n   n=" << n << "\n\n\n\n";
            n++;
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log has a max size cap
TEST_F(RamLogTest, MaxSize) {
    RamLog* ramlog = RamLog::get("TestRamlog3");

    std::vector<std::string> lines;

    constexpr size_t testLines = 2000;
    constexpr size_t longStringLength = 2048;

    std::string longStr(longStringLength, 'a');

    // Write enough lines to trigger wrapping and trimming
    for (size_t i = 0; i < testLines; ++i) {
        auto s = fmt::format("{}{}", 10000 + i, longStr);
        lines.push_back(s);
        ramlog->write(s);
    }

    constexpr size_t linesToFit = (1024 * 1024) / (5 + longStringLength);

    lines.erase(lines.begin(), lines.begin() + (testLines - linesToFit));

    // Verify we keep just enough lines that fit
    {
        RamLog::LineIterator iter(ramlog);
        EXPECT_EQ(iter.getTotalLinesWritten(), 2000UL);
        for (const auto& line : lines) {
            ASSERT_EQ(iter.next(), line);
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log has a max size cap
TEST_F(RamLogTest, MaxSizeAltMaxLinesMaxSize) {
    constexpr size_t testLines = 2000;
    constexpr size_t longStringLength = 2048;
    constexpr size_t fullStringLength = 2048 + 5;

    constexpr size_t alternativeMaxLines = 2048;
    constexpr size_t alternativeMaxSizeBytes = 1024 * 1024 + fullStringLength;
    RamLog::setGlobalMaxLines(alternativeMaxLines);
    RamLog::setGlobalMaxSizeBytes(alternativeMaxSizeBytes);
    RamLog* ramlog = RamLog::get("TestRamlog3Alt");

    std::vector<std::string> lines;

    std::string longStr(longStringLength, 'a');

    // Write enough lines to trigger wrapping and trimming
    for (size_t i = 0; i < testLines; ++i) {
        auto s = fmt::format("{}{}", 10000 + i, longStr);
        lines.push_back(s);
        ramlog->write(s);
    }

    constexpr size_t linesToFit = alternativeMaxSizeBytes / fullStringLength;

    lines.erase(lines.begin(), lines.begin() + (testLines - linesToFit));

    // Verify we keep just enough lines that fit
    {
        RamLog::LineIterator iter(ramlog);
        EXPECT_EQ(iter.getTotalLinesWritten(), 2000UL);
        for (const auto& line : lines) {
            ASSERT_EQ(iter.next(), line);
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log handles really large lines
TEST_F(RamLogTest, GiantLine) {
    RamLog* ramlog = RamLog::get("TestRamlog4");

    std::vector<std::string> lines;

    constexpr size_t testLines = 5000;

    // Write enough lines to trigger wrapping
    for (size_t i = 0; i < testLines; ++i) {
        ramlog->write(fmt::to_string(i));
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
        EXPECT_EQ(iter.getTotalLinesWritten(), testLines + 2);
        for (const auto& line : lines) {
            ASSERT_EQ(iter.next(), line);
        }
    }

    ramlog->clear();
}

// Positive: Test that the ram log handles really large lines
TEST_F(RamLogTest, GiantLineAltMaxLinesMaxSize) {
    constexpr size_t alternativeMaxLines = 1024;
    constexpr size_t alternativeMaxSizeBytes = 2 * 1024 * 1024;
    RamLog::setGlobalMaxLines(alternativeMaxLines);
    RamLog::setGlobalMaxSizeBytes(alternativeMaxSizeBytes);
    RamLog* ramlog = RamLog::get("TestRamlog4Alt");

    std::vector<std::string> lines;

    constexpr size_t testLines = 5000;

    // Write enough lines to trigger wrapping
    for (size_t i = 0; i < testLines; ++i) {
        ramlog->write(fmt::to_string(i));
    }

    auto s = fmt::to_string(testLines);
    lines.push_back(s);
    ramlog->write(s);

    std::string bigStr(2048 * 1024 + 128, 'a');
    lines.push_back(bigStr);
    ramlog->write(bigStr);

    // Verify we keep 2 lines
    {
        RamLog::LineIterator iter(ramlog);
        EXPECT_EQ(iter.getTotalLinesWritten(), testLines + 2);
        for (const auto& line : lines) {
            ASSERT_EQ(iter.next(), line);
        }
    }

    ramlog->clear();
}

TEST_F(RamLogTest, SetGlobalMaxLinesUnchanged) {
    auto* ramlog = RamLog::get("TestRamlogGlobalMaxLinesUnchanged");
    ramlog->clear();

    RamLog::setGlobalMaxLines(16);

    for (size_t i = 0; i < 10; ++i) {
        ramlog->write(fmt::to_string(i));
    }

    const auto before = readRamLogLines(ramlog);

    RamLog::setGlobalMaxLines(16);

    EXPECT_EQ(RamLog::getGlobalMaxLines(), 16);
    EXPECT_EQ(ramlog->getMaxLines(), 16);
    EXPECT_EQ(readRamLogLines(ramlog), before);
}

TEST_F(RamLogTest, SetGlobalMaxLinesSmaller) {
    auto* ramlog = RamLog::get("TestRamlogGlobalMaxLinesSmaller");
    ramlog->clear();

    RamLog::setGlobalMaxLines(8);

    for (size_t i = 0; i < 10; ++i) {
        ramlog->write(fmt::format("line{}", i));
    }

    RamLog::setGlobalMaxLines(3);

    EXPECT_EQ(RamLog::getGlobalMaxLines(), 3);
    EXPECT_EQ(ramlog->getMaxLines(), 3);
    const std::vector<std::string> expected{"line8", "line9"};
    EXPECT_EQ(readRamLogLines(ramlog), expected);
}

TEST_F(RamLogTest, SetGlobalMaxLinesLarger) {
    auto* ramlog = RamLog::get("TestRamlogGlobalMaxLinesLarger");
    ramlog->clear();

    RamLog::setGlobalMaxLines(8);

    for (size_t i = 0; i < 5; ++i) {
        ramlog->write(fmt::format("line{}", i));
    }

    RamLog::setGlobalMaxLines(12);

    EXPECT_EQ(RamLog::getGlobalMaxLines(), 12);
    EXPECT_EQ(ramlog->getMaxLines(), 12);
    const std::vector<std::string> expected{"line0", "line1", "line2", "line3", "line4"};
    EXPECT_EQ(readRamLogLines(ramlog), expected);
}

TEST_F(RamLogTest, MaxSizeRemovesMinimalLines) {
    auto* ramlog = RamLog::get("TestMaxSizeRemovesMinimalLines");
    ramlog->clear();

    RamLog::setGlobalMaxSizeBytes(128);
    ramlog->write("1");
    ramlog->write("A longer string");
    ramlog->write(std::string(40, 'a'));
    // 56 bytes are stored at this point

    // write a string that is 1 byte too long. The "1" line will be removed.
    ramlog->write(std::string(72, 'b'));
    EXPECT_EQ(readRamLogLines(ramlog).size(), 3);

    // write a string that is 1 byte longer than the first string.
    // 2 lines will be removed to add one, for a net loss of 1.
    ramlog->write(std::string(16, 'c'));
    EXPECT_EQ(readRamLogLines(ramlog).size(), 2);
}

TEST_F(RamLogTest, SetGlobalMaxSizeBytesUnchanged) {
    auto* ramlog = RamLog::get("TestRamlogGlobalMaxSizeUnchanged");
    ramlog->clear();

    constexpr size_t kMaxSizeBytes = 128;
    RamLog::setGlobalMaxSizeBytes(kMaxSizeBytes);

    ramlog->write("aaaa");
    ramlog->write("bbbb");
    ramlog->write("cccc");

    const auto before = readRamLogLines(ramlog);

    RamLog::setGlobalMaxSizeBytes(kMaxSizeBytes);

    EXPECT_EQ(RamLog::getGlobalMaxSizeBytes(), kMaxSizeBytes);
    EXPECT_EQ(ramlog->getMaxSizeBytes(), kMaxSizeBytes);
    EXPECT_EQ(readRamLogLines(ramlog), before);
}

TEST_F(RamLogTest, SetGlobalMaxSizeBytesSmaller) {
    auto* ramlog = RamLog::get("TestRamlogGlobalMaxSizeSmaller");
    ramlog->clear();

    RamLog::setGlobalMaxSizeBytes(200);

    ramlog->write(std::string(40, 'a'));
    ramlog->write(std::string(40, 'b'));
    ramlog->write(std::string(40, 'c'));
    ramlog->write(std::string(40, 'd'));

    RamLog::setGlobalMaxSizeBytes(100);
    EXPECT_EQ(RamLog::getGlobalMaxSizeBytes(), 100);
    EXPECT_EQ(ramlog->getMaxSizeBytes(), 100);
    EXPECT_EQ(readRamLogLines(ramlog).size(), 2);

    // setting smaller than the most recent line should leave one line behind
    RamLog::setGlobalMaxSizeBytes(30);
    EXPECT_EQ(RamLog::getGlobalMaxSizeBytes(), 30);
    EXPECT_EQ(ramlog->getMaxSizeBytes(), 30);
    EXPECT_EQ(readRamLogLines(ramlog).size(), 1);
}

TEST_F(RamLogTest, SetGlobalMaxSizeBytesLarger) {
    auto* ramlog = RamLog::get("TestRamlogGlobalMaxSizeLarger");
    ramlog->clear();

    RamLog::setGlobalMaxSizeBytes(50);

    ramlog->write(std::string(20, 'a'));
    ramlog->write(std::string(20, 'b'));

    const auto before = readRamLogLines(ramlog);

    RamLog::setGlobalMaxSizeBytes(256);

    EXPECT_EQ(RamLog::getGlobalMaxSizeBytes(), 256);
    EXPECT_EQ(ramlog->getMaxSizeBytes(), 256);
    EXPECT_EQ(readRamLogLines(ramlog), before);
}

TEST_F(RamLogTest, GetIfExists) {
    EXPECT_EQ(RamLog::getIfExists("TestRamlogGetIfExistsMissing"), nullptr);

    auto* created = RamLog::get("TestRamlogGetIfExists");
    created->write("hello");

    EXPECT_EQ(RamLog::getIfExists("TestRamlogGetIfExists"), created);
}

TEST_F(RamLogTest, GetNames) {
    auto* ramlog = RamLog::get("TestRamlogGetNames");
    ramlog->clear();

    const auto emptyNames = RamLog::getNames();
    EXPECT_TRUE(emptyNames.end() ==
                std::find(emptyNames.begin(), emptyNames.end(), "TestRamlogGetNames"));

    ramlog->write("present");

    const auto names = RamLog::getNames();
    EXPECT_TRUE(names.end() != std::find(names.begin(), names.end(), "TestRamlogGetNames"));
}

TEST_F(RamLogTest, Clear) {
    auto* ramlog = RamLog::get("TestRamlogClear");
    ramlog->write("one");
    ramlog->write("two");

    ramlog->clear();

    EXPECT_TRUE(readRamLogLines(ramlog).empty());

    RamLog::LineIterator iter(ramlog);
    EXPECT_EQ(iter.getTotalLinesWritten(), 0);
}

TEST_F(RamLogTest, WriteEmptyStringIncrementsTotalLinesWritten) {
    auto* ramlog = RamLog::get("TestRamlogEmptyWrite");
    ramlog->clear();

    ramlog->write("");
    ramlog->write("data");

    {
        RamLog::LineIterator iter(ramlog);
        EXPECT_EQ(iter.getTotalLinesWritten(), 2);
    }
    EXPECT_EQ(readRamLogLines(ramlog), std::vector<std::string>{"data"});
}

TEST_F(RamLogTest, LineIteratorApis) {
    auto* ramlog = RamLog::get("TestRamlogLineIterator");
    ramlog->clear();

    ramlog->write("first");
    ramlog->write("second");

    RamLog::LineIterator iter(ramlog);
    EXPECT_EQ(iter.len(), 2);
    ASSERT_TRUE(iter.more());
    EXPECT_EQ(iter.next(), "first");
    ASSERT_TRUE(iter.more());
    EXPECT_EQ(iter.next(), "second");
    ASSERT_FALSE(iter.more());
    EXPECT_EQ(iter.next(), "");
}

}  // namespace

}  // namespace mongo::logv2
