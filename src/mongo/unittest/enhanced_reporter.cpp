/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/unittest/enhanced_reporter.h"

#include "mongo/base/string_data.h"
#include "mongo/logv2/composite_backend.h"
#include "mongo/logv2/domain_filter.h"
#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_capture_backend.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/uassert_sink.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/pcre.h"
#include "mongo/util/signal_handlers_synchronous.h"

#include <array>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>

#include <boost/log/core/core.hpp>
#include <boost/optional.hpp>
#include <boost/shared_ptr.hpp>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/xchar.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef MONGO_CONFIG_DEV_STACKTRACE
#include "mongo/logv2/dev_stacktrace_formatter.h"
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT mongo::logv2::LogComponent::kTest

namespace mongo::unittest {

namespace {

namespace posix_compat {

#if _WIN32
// TODO(gtest): Reenable output buffering on Windows.
#define ENHANCED_REPORTER_BUFFERING_ENABLED 0
#else
#define ENHANCED_REPORTER_BUFFERING_ENABLED 1
#endif

#if _WIN32

int openReadOnly(const char* path) {
    return ::_open(path, _O_RDONLY);
}
int read(int fd, void* buffer, size_t len) {
    return ::_read(fd, buffer, len);
}
int write(int fd, const void* buffer, size_t len) {
    return ::_write(fd, buffer, len);
}
int close(int fd) {
    return ::_close(fd);
}

#else  // not _WIN32

int openReadOnly(const char* path) {
    return ::open(path, O_RDONLY);
}
ssize_t read(int fd, void* buffer, size_t len) {
    return ::read(fd, buffer, len);
}
ssize_t write(int fd, const void* buffer, size_t len) {
    return ::write(fd, buffer, len);
}
int close(int fd) {
    return ::close(fd);
}

#endif  // not _WIN32

}  // namespace posix_compat

constexpr auto kBold = fmt::emphasis::bold;  // Also brightens on some but not all terms.
constexpr auto kDim = fmt::emphasis::faint;
constexpr auto kItalic = fmt::emphasis::italic;
constexpr auto kUnderline = fmt::emphasis::underline;

// Colors that can be hard to read are commented out to prevent usage.
// constexpr auto kBlack = fmt::terminal_color::black;
constexpr auto kRed = fg(fmt::terminal_color::red);
constexpr auto kGreen = fg(fmt::terminal_color::green);
constexpr auto kYellow = fg(fmt::terminal_color::yellow);
// constexpr auto kBlue = fg(fmt::terminal_color::blue);
constexpr auto kMagenta = fg(fmt::terminal_color::magenta);
constexpr auto kCyan = fg(fmt::terminal_color::cyan);
// constexpr auto kWhite = fmt::terminal_color::white;

// Presets for certain types of output to ensure uniformity in styling.
constexpr auto kSourceLocation = kDim | kUnderline;

// Note: summary() is message() without the stack trace.
// And by then we should have our fancy cpptrace stack traces to use instead.
// When we have them, only print them if there are frames on top of `TestBody()`.
// TODO(gtest): Once we have cpptrace stack traces, so we should print them if
// there are frames on top of `TestBody()`.
StringData stripMessage(StringData message) {
    StringData s = message;
    static constexpr StringData kMsgPrefix = "Succeeded\n";
    if (s.starts_with(kMsgPrefix)) {
        s.remove_prefix(kMsgPrefix.length());
    }
    // Skip leading newlines. These happen in usages of FAIL() << "message",
    // such as in unexpected calls in gmock.
    while (s.starts_with("\n"))
        s.remove_prefix(1);

    if (s.empty()) {
        return message;
    }
    return s;
}

#if ENHANCED_REPORTER_BUFFERING_ENABLED

/** snprintf, but returns the number of characters that were actually written. */
template <typename... Args>
size_t snprintfTruncated(char* buffer, size_t n, const char* format, Args... args) {
    int rc = snprintf(buffer, n, format, std::forward<Args>(args)...);

    // Truncate to the buffer size - 1 to account for the NULL terminator.
    if (rc > static_cast<int>(n)) {
        rc = n;
    }
    // Treat any errors as "nothing got written."
    if (rc < 0) {
        rc = 0;
    }
    return rc;
}

/**
 * Encapsulates the result of a system call. Safe to construct inside a signal handler. Messages are
 * capped to 256 bytes.
 */
class SystemCallResult {
public:
    /** Default constructor means successful system call. */
    SystemCallResult() = default;

    /** For failed system calls. */
    template <typename... Args>
    SystemCallResult(std::error_code ec, StringData message) : _ec{ec} {
        _size = message.copy(_message.data(), _message.size());
    }

    /**
     * Throw a std::system_error if this result represents a failed system call. NOT safe to call
     * inside a signal handler.
     */
    void throwIfNotOK() {
        if (!_ec) {
            return;
        }
        throw std::system_error(*_ec, std::string{_message.data(), _size});
    }

    /**
     * Prints error details to stdout if this result represents a failed system call. Safe to call
     * inside a signal handler.
     */
    void printIfNotOK() {
        if (!_ec) {
            return;
        }

        static constexpr StringData header = "ERROR: Couldn't dump buffered logs: ";
        static constexpr StringData footer = " failed";

        if (posix_compat::write(details::stdoutFileNo, header.data(), header.size()) <
            static_cast<ssize_t>(header.size())) {
            return;
        }
        if (posix_compat::write(details::stdoutFileNo, _message.data(), _size) <
            static_cast<ssize_t>(_size)) {
            return;
        }
        if (posix_compat::write(details::stdoutFileNo, footer.data(), footer.size()) <
            static_cast<ssize_t>(footer.size())) {
            return;
        }
        // TODO(gtest): find a way to print the error code in signal-handler-safe fashion.
    }

private:
    boost::optional<std::error_code> _ec;
    std::array<char, 256> _message;
    size_t _size{0};
};

/**
 * Copies contents of a file to stdout. Safe to call from inside signal handlers.
 * TODO(gtest): find a way to augment the context of the error messages in signal-handler-safe
 * fashion.
 */
SystemCallResult printFileToStdoutSignalHandlerSafe(const std::filesystem::path& path) noexcept {
    char buffer[4096];
    auto pathStr = path.c_str();

    int src = posix_compat::openReadOnly(pathStr);
    if (src < 0) {
        auto ec = lastSystemError();
        constexpr StringData message = "open";
        return SystemCallResult(ec, message);
    }

    for (;;) {
        auto bytesRead = posix_compat::read(src, buffer, sizeof(buffer));
        if (bytesRead < 0) {
            auto ec = lastSystemError();
            posix_compat::close(src);
            constexpr StringData message = "read";
            return SystemCallResult(ec, message);
        }
        if (bytesRead == 0) {
            break;
        }

        auto bytesWritten = posix_compat::write(details::stdoutFileNo, buffer, bytesRead);
        if (bytesWritten < 0) {
            auto ec = lastSystemError();
            posix_compat::close(src);
            constexpr StringData message = "write";
            return SystemCallResult(ec, message);
        }
        if (bytesRead != bytesWritten) {
            posix_compat::close(src);
            constexpr StringData message = "write";
            return SystemCallResult(systemError(0), message);
        }
    }

    posix_compat::close(src);
    return {};
}

#endif  // ENHANCED_REPORTER_BUFFERING_ENABLED

bool addExceptionInfo(auto&& buf) {
    if (!std::current_exception())
        return false;

    auto diag = [&](StringData type, StringData info) {
        fmt::println(buf, "Exception encountered, extra info:");
        fmt::println(buf, "{}: {}", type, info);
        fmt::println(buf, "");
    };

    try {
        throw;
    } catch (const DBException& e) {
        diag("DBException", e.toString());
    } catch (const boost::exception& e) {
        diag("boost::exception", boost::diagnostic_information(e));
    } catch (const std::exception& e) {
        diag("std::exception", e.what());
    } catch (...) {
        diag("unknown", "?");
        return false;
    }
    return true;
}

}  // namespace

class EnhancedReporter::Impl {
public:
    explicit Impl(std::unique_ptr<testing::TestEventListener> defaultListener)
        : _defaultListener(std::move(defaultListener)) {}

    Impl(std::unique_ptr<testing::TestEventListener> defaultListener, Options options)
        : _defaultListener(std::move(defaultListener)), _options(std::move(options)) {}

    void OnTestProgramStart(const testing::UnitTest& unitTest);

    void OnTestIterationStart(const testing::UnitTest& unitTest, int iteration);

    void OnTestStart(const testing::TestInfo& testInfo);

    void OnTestPartResult(const testing::TestPartResult& res);

    void OnTestEnd(const testing::TestInfo& testInfo);

    void OnTestIterationEnd(const testing::UnitTest& unitTest, int iteration);

    void enable();
    void disable();

    void dumpBufferedOutputForSignalHandler();

private:
#if ENHANCED_REPORTER_BUFFERING_ENABLED
    /**
     * Diverts logs from stdout and captures them internally. Can emit or drop them. Original
     * logging behavior cannot be restored.
     */
    class LogDiverter {
    public:
        using LogDiverterBackend =
            logv2::CompositeBackend<logv2::LogCaptureBackend, logv2::UserAssertSink>;

        explicit LogDiverter(EnhancedReporter::Impl* reporter) {
            boost::log::core::get()->remove_all_sinks();
            auto sink = boost::make_shared<LogDiverterBackend>(
                boost::make_shared<logv2::LogCaptureBackend>(std::make_unique<Listener>(reporter),
                                                             false),
                boost::make_shared<logv2::UserAssertSink>());
            sink->lockedBackend<0>()->setEnabled(true);
            _sink = boost::make_shared<boost::log::sinks::unlocked_sink<LogDiverterBackend>>(
                std::move(sink));
            _sink->set_filter(logv2::AllLogsFilter(logv2::LogManager::global().getGlobalDomain()));
#ifdef MONGO_CONFIG_DEV_STACKTRACE
            logv2::LogDomainGlobal::ConfigurationOptions opts{};
            _sink->set_formatter(
                logv2::DevStacktraceFormatter(opts.maxAttributeSizeKB, opts.timestampFormat));
#else
            _sink->set_formatter(logv2::JSONFormatter());
#endif
            boost::log::core::get()->add_sink(_sink);
        }

        void enable() {
            if (std::exchange(_enabled, true))
                return;
            boost::log::core::get()->add_sink(_sink);
        }

        void disable() {
            if (!std::exchange(_enabled, false))
                return;
            boost::log::core::get()->remove_sink(_sink);
        }

    private:
        class Listener : public logv2::LogLineListener {
        public:
            explicit Listener(EnhancedReporter::Impl* reporter) : _reporter(reporter) {}
            void accept(const std::string& line) override {
                stdx::lock_guard lk(_reporter->_mutex);
                fmt::print(_reporter->_buffer, "{}", line);
            }

        private:
            Impl* _reporter;
        };

        boost::shared_ptr<boost::log::sinks::unlocked_sink<LogDiverterBackend>> _sink;
        bool _enabled{true};
    };

    /** Throws a std::system_error based on the saved error of the last system call. */
    void _throwLastSystemError(StringData expr) {
        auto ec = lastSystemError();
        throw std::system_error(ec, fmt::format("{} failed: {}", expr, errorMessage(ec)));
    }

    /** Clears any buffered output. Directs logs and enhanced reporter output to a temp file. */
    void _startBuffering(WithLock lk) {
        invariant(!std::exchange(_buffering, true));
        invariant(_buffer == stdout);
        fflush(_buffer);
        _truncateBuffer(lk);
        _buffer = fopen(_tempPath.string().c_str(), "w");
        if (!_buffer)
            _throwLastSystemError(fmt::format("fopen({})", _tempPath.string()));
        // We turn off buffering so that in the event of a process-terminating signal, we have as
        // much context as possible in the buffer.
        setvbuf(_buffer, NULL, _IONBF, 0);
    }

    /** Dumps any buffered output if flush is true. Directs logs and output to stdout. */
    void _stopBuffering(WithLock lk, bool flush) {
        if (!std::exchange(_buffering, false)) {
            return;
        }
        invariant(fclose(_buffer) == 0);
        _buffer = stdout;
        if (flush) {
            printFileToStdoutSignalHandlerSafe(_tempPath).throwIfNotOK();
        }
        _truncateBuffer(lk);
    }

    /**
     * Dumps any buffered output and redirects logs and output to stdout. Can be safely called
     * inside a signal handler. May lose concurrently emitted output if _mutex is not held. Do not
     * call unless the process is imminently dying; output may be duplicated if execution resumes
     * normally.
     */
    void _stopBufferingSignalSafe() {
        if (!std::exchange(_buffering, false)) {
            return;
        }
        _buffer = stdout;
        printFileToStdoutSignalHandlerSafe(_tempPath).printIfNotOK();
    }

    /** Truncates the temp file buffer. */
    void _truncateBuffer(WithLock) {
        std::error_code ec;
        std::filesystem::remove(_tempPath, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            throw std::system_error(ec);
        }
    }
#endif  // ENHANCED_REPORTER_BUFFERING_ENABLED

    fmt::text_style _style(fmt::text_style style) {
        return _options.useColor ? style : fmt::text_style{};
    }
    template <typename T>
    auto _style(fmt::text_style style, const T& str) {
        return fmt::styled(str, _style(style));
    }

    /** Style message by highlighting strings. */
    void _styleMessage(std::string& message) {
        // Add bold for most important prefixes. This catches a subset of the
        // lines highlighted by the next regex.
        // TODO(gtest): might be better to process linewise and add some logic that examines
        // the prefix to decide how to highlight it. But for now, using regex substitution is
        // simpler.
        // TODO(gtest): consider whether alternatives to pcre may be more advantageous.
        {
            static const auto re =
                pcre::Regex(R"(^( *(?:Actual|Which is|Function call)): (.*)$)", pcre::MULTILINE);
            static const auto replacement =
                fmt::format("{}: {}", _style(kBold | kCyan, "$1"), _style(kBold, "$2"));
            re.substitute(replacement, &message, pcre::SUBSTITUTE_GLOBAL);
        }

        {
            // Highlight other info prefixes like "Value of: " or "Expected: "
            static const auto re =
                pcre::Regex(R"(^( *(?: ?[a-zA-Z])+(?: #\d+)?): )", pcre::MULTILINE);
            static const auto replacement = fmt::format("{}: ", _style(kCyan, "$1"));
            re.substitute(replacement, &message, pcre::SUBSTITUTE_GLOBAL);
        }

        {
            // Highlight the output of CAPTURE(x) "file.cpp:123: x := 42"
            static const auto re =
                pcre::Regex(R"(^([-\w+/\.]+:\d+): (.*) := (.*)$)", pcre::MULTILINE);
            static const auto replacement = fmt::format("{}: {} := {}",
                                                        _style(kSourceLocation, "$1"),
                                                        _style(kBold, "$2"),
                                                        _style(kBold, "$3"));
            re.substitute(replacement, &message, pcre::SUBSTITUTE_GLOBAL);
        }

        {
            // Highlight other file/line prefixes like "file.cpp:123:
            static const auto re = pcre::Regex(R"(^([-\w+/\.]+:\d+):)", pcre::MULTILINE);
            const auto replacement = fmt::format("{}:", _style(kSourceLocation, "$1"));
            re.substitute(replacement, &message, pcre::SUBSTITUTE_GLOBAL);
        }
    }

    // Guards _buffer.
    stdx::timed_mutex _mutex;  // NOLINT
    std::unique_ptr<testing::TestEventListener> _defaultListener;
    Options _options;
    const testing::UnitTest* _env;
    const testing::TestInfo* _currentTest;
    int _testCount{0};
    FILE* _buffer{stdout};
#if ENHANCED_REPORTER_BUFFERING_ENABLED
    bool _buffering{false};
    LogDiverter _logDiverter{this};
    std::filesystem::path _tempPath =
        std::filesystem::temp_directory_path() / "enhanced_reporter_buffer";
#endif  // ENHANCED_REPORTER_BUFFERING_ENABLED
};

void EnhancedReporter::Impl::OnTestProgramStart(const testing::UnitTest& unitTest) {
    stdx::lock_guard lk(_mutex);
    auto note = [&](StringData message) {
        fmt::println(_buffer, "{}{}", _style(kCyan, "NOTE: "), _style(kDim, message));
    };

    _env = &unitTest;
    _defaultListener->OnTestProgramStart(unitTest);
    note("Using mongo output format. Set --enhancedReporter=false to disable.");
    // TODO(gtest): Consider removing this debug output before merging to master.
    note(fmt::format("Stdout is {} TTY and {}running in bazel. {} colorize and {} emit a spinner.",
                     details::stdoutIsTty() ? "a" : "not a",
                     details::inBazelTest() ? "" : "not ",
                     _options.useColor ? "Will" : "Will not",
                     _options.useSpinner ? "will" : "will not"));
    if (!_options.showEachTest)
        note("Hiding output for passing tests. Pass --showEachTest to see everything.");
}

void EnhancedReporter::Impl::OnTestIterationStart(const testing::UnitTest& unitTest,
                                                  int iteration) {
    stdx::lock_guard lk(_mutex);
    fmt::print(_buffer,
               _style(kCyan),
               "Running {} tests in {} suites [ {} of {} ]",
               unitTest.total_test_count(),
               unitTest.total_test_suite_count(),
               iteration,
               GTEST_FLAG_GET(repeat));
    fmt::println(_buffer, "");
    _testCount = 0;
}

void EnhancedReporter::Impl::OnTestStart(const testing::TestInfo& testInfo) {
    stdx::lock_guard lk(_mutex);
#if ENHANCED_REPORTER_BUFFERING_ENABLED
    if (!_options.showEachTest) {
        _startBuffering(lk);
    }
#endif  // ENHANCED_REPORTER_BUFFERING_ENABLED
    _currentTest = &testInfo;
    ++_testCount;

    // Print the header
    static const auto bar = fmt::format("{:=>60}", "");
    auto row = [&](StringData label, StringData value) {
        static const int pad = 13;
        fmt::println(_buffer, "{: >{}}: {}", _style(kYellow, label), pad, value);
    };
    fmt::println(_buffer, "   ");  // Clear the spinner, but leave the count.
    fmt::println(_buffer, "{}", _style(kCyan, bar));
    fmt::print(
        _buffer, _style(kSourceLocation), "{}:{}", _currentTest->file(), _currentTest->line());
    fmt::println(_buffer, "");
    row("TEST SUITE", _currentTest->test_suite_name());
    row("TEST CASE", _currentTest->name());
    if (auto param = _currentTest->type_param()) {
        row("TYPE PARAM", param);
    }
    if (auto param = _currentTest->value_param()) {
        row("VALUE PARAM", param);
    }
    fmt::println(_buffer, "");

    if (!_options.showEachTest && _options.useSpinner) {
        static constexpr StringData spinner = R"(|/-\)";
        // Note: adding \r rather than \n to seek to begining of line.
        // Anything printed after this will overwrite the spinner, so
        // keep it short so that it will be fully overwritten.
        // This will be less important once we capture mongo logs.
        // TODO(gtest): add compact test name
        fmt::print("  {} {}\r",
                   _style(kBold, spinner[_testCount % spinner.size()]),
                   _style(kDim, fmt::format("[{} of {}]", _testCount, _env->test_to_run_count())));
        fflush(stdout);
    }
}

void EnhancedReporter::Impl::OnTestPartResult(const testing::TestPartResult& res) {
    stdx::lock_guard lk(_mutex);

    // If this is a failure and we've been suppressing log output, dump out the buffer and direct
    // logs to stdout.
#if ENHANCED_REPORTER_BUFFERING_ENABLED
    if (!_options.showEachTest &&
        (res.type() == testing::TestPartResult::kNonFatalFailure ||
         res.type() == testing::TestPartResult::kFatalFailure)) {
        _stopBuffering(lk, /* flush */ true);
    }
#endif  // ENHANCED_REPORTER_BUFFERING_ENABLED

    // Print failure details
    if (res.file_name()) {
        fmt::println(
            _buffer,
            "{}:",
            fmt::format(_style(kBold | kUnderline), "{}:{}", res.file_name(), res.line_number()));
    } else {
        fmt::println(_buffer, "Unknown location:");
    }
    switch (res.type()) {
        case testing::TestPartResult::kSuccess:
            fmt::print(_buffer, _style(kCyan), "INFO: ");
            break;
        case testing::TestPartResult::kNonFatalFailure:
        case testing::TestPartResult::kFatalFailure:
            fmt::print(_buffer, _style(kRed), "FAIL: ");
            break;
        case testing::TestPartResult::kSkip:
            fmt::print(_buffer, _style(kYellow), "SKIP: ");
            break;
    }

    if (addExceptionInfo(_buffer))
        return;

    auto message = std::string{stripMessage(res.message())};
    if (_options.useColor) {
        _styleMessage(message);
    }
    fmt::println(_buffer, "{}", message);

    // TODO(gtest): check if there is a caught exception using current_exception() and
    // extract more data, such as the code for DBException, or the precise type for other
    // exceptions. If summary() is just about the exception we can replace it, otherwise,
    // we can print extra info after the summary.
}

void EnhancedReporter::Impl::OnTestEnd(const testing::TestInfo& testInfo) {
    stdx::lock_guard lk(_mutex);
    auto res = testInfo.result();
    auto disposition = [&]() {
        if (res->Failed()) {
            return _style(kBold | kRed, "FAIL");
        }
        if (res->Skipped()) {
            return _style(kYellow, "SKIP");
        }
        return _style(kBold | kGreen, "PASS");
    }();
    fmt::println(_buffer,
                 "{}{} ({} ms)",
                 _style(kYellow, "TEST RESULT: "),
                 std::move(disposition),
                 res->elapsed_time());
    fmt::println(_buffer, "");  // Add a blank line to separate tests.
#if ENHANCED_REPORTER_BUFFERING_ENABLED
    if (!_options.showEachTest) {
        _stopBuffering(lk, /* flush */ res->Failed());
    }
#endif  // ENHANCED_REPORTER_BUFFERING_ENABLED
}

void EnhancedReporter::Impl::OnTestIterationEnd(const testing::UnitTest& unitTest, int iteration) {
    stdx::lock_guard lk(_mutex);
    fmt::println(_buffer,
                 "{} / {} / {} / {} [ {} of {} ]",
                 fmt::format(_style(kCyan), "{} TOTAL", unitTest.total_test_count()),
                 fmt::format(_style(kGreen), "{} PASS", unitTest.successful_test_count()),
                 fmt::format(_style(kRed), "{} FAIL", unitTest.failed_test_count()),
                 fmt::format(_style(kYellow), "{} SKIP", unitTest.skipped_test_count()),
                 iteration,
                 GTEST_FLAG_GET(repeat));
}

void EnhancedReporter::Impl::enable() {
#if ENHANCED_REPORTER_BUFFERING_ENABLED
    _logDiverter.enable();
#endif
}

void EnhancedReporter::Impl::disable() {
#if ENHANCED_REPORTER_BUFFERING_ENABLED
    _logDiverter.disable();
#endif
}

void EnhancedReporter::Impl::dumpBufferedOutputForSignalHandler() {
#if ENHANCED_REPORTER_BUFFERING_ENABLED
    // Lock the mutex with best effort. We can't deadlock in a signal handler and it's more valuable
    // to emit potentially mangled output with a disclaimer than to give up.
    bool locked = _mutex.try_lock_for(std::chrono::milliseconds{100});  // NOLINT
    if (!locked) {
        fmt::println(
            "ERROR: Could not acquire logging mutex for 100 milliseconds. Dumping buffered logs "
            "anyways.");
    }
    _stopBufferingSignalSafe();
    if (locked) {
        _mutex.unlock();
    }
#endif  // ENHANCED_REPORTER_BUFFERING_ENABLED
}


EnhancedReporter::EnhancedReporter(std::unique_ptr<testing::TestEventListener> defaultListener)
    : _impl(std::make_unique<Impl>(std::move(defaultListener))) {}

EnhancedReporter::EnhancedReporter(std::unique_ptr<testing::TestEventListener> defaultListener,
                                   EnhancedReporter::Options options)
    : _impl(std::make_unique<Impl>(std::move(defaultListener), std::move(options))) {}

EnhancedReporter::~EnhancedReporter() = default;

void EnhancedReporter::OnTestProgramStart(const testing::UnitTest& unitTest) {
    _impl->OnTestProgramStart(unitTest);
}
void EnhancedReporter::OnTestIterationStart(const testing::UnitTest& unitTest, int iteration) {
    _impl->OnTestIterationStart(unitTest, iteration);
}
void EnhancedReporter::OnTestStart(const testing::TestInfo& testInfo) {
    _impl->OnTestStart(testInfo);
}
void EnhancedReporter::OnTestPartResult(const testing::TestPartResult& res) {
    _impl->OnTestPartResult(res);
}
void EnhancedReporter::OnTestEnd(const testing::TestInfo& testInfo) {
    _impl->OnTestEnd(testInfo);
}
void EnhancedReporter::OnTestIterationEnd(const testing::UnitTest& unitTest, int iteration) {
    _impl->OnTestIterationEnd(unitTest, iteration);
}
void EnhancedReporter::enable() {
    _impl->enable();
}
void EnhancedReporter::disable() {
    _impl->disable();
}
void EnhancedReporter::dumpBufferedOutputForSignalHandler() {
    _impl->dumpBufferedOutputForSignalHandler();
}

}  // namespace mongo::unittest
