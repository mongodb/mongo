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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"

#include <boost/log/core.hpp>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <functional>
#include <iostream>
#include <map>
#include <memory>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/db/server_options.h"
#include "mongo/logger/console_appender.h"
#include "mongo/logger/log_manager.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/logger/message_log_domain.h"
#include "mongo/logv2/bson_formatter.h"
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_capture_backend.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace unittest {
namespace {

bool stringContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

logger::MessageLogDomain* unittestOutput() {
    static const auto p = logger::globalLogManager()->getNamedDomain("unittest");
    return p;
}

/** Each map key is owned by its corresponding Suite object. */
auto& suitesMap() {
    static std::map<StringData, std::shared_ptr<Suite>> m;
    return m;
}

}  // namespace

logger::LogstreamBuilderDeprecated log() {
    return LogstreamBuilderDeprecated(
        unittestOutput(), getThreadName(), logger::LogSeverity::Log());
}

logger::LogstreamBuilderDeprecated warning() {
    return LogstreamBuilderDeprecated(
        unittestOutput(), getThreadName(), logger::LogSeverity::Warning());
}

void setupTestLogger() {
    unittestOutput()->attachAppender(
        std::make_unique<logger::ConsoleAppender<logger::MessageLogDomain::Event>>(
            std::make_unique<logger::MessageEventDetailsEncoder>()));
}

MONGO_INITIALIZER_WITH_PREREQUISITES(UnitTestOutput, ("GlobalLogManager", "default"))
(InitializerContext*) {
    setupTestLogger();
    return Status::OK();
}

class Result {
public:
    Result(const std::string& name)
        : _name(name), _rc(0), _tests(0), _fails(), _asserts(0), _millis(0) {}

    std::string toString() const {
        std::ostringstream ss;
        using namespace fmt::literals;
        ss << "{:<40s} | tests: {:4d} | fails: {:4d} | "
              "assert calls: {:10d} | time secs: {:6.3f}\n"
              ""_format(_name, _tests, _fails.size(), _asserts, _millis * 1e-3);

        for (const auto& i : _messages) {
            ss << "\t" << i << '\n';
        }

        return ss.str();
    }

    int rc() {
        return _rc;
    }

    std::string _name;

    int _rc;
    int _tests;
    std::vector<std::string> _fails;
    int _asserts;
    int _millis;
    std::vector<std::string> _messages;
};

namespace {

/**
 * This unsafe scope guard allows exceptions in its destructor. Thus, if it goes out of scope when
 * an exception is active and the guard function also throws an exception, the program will call
 * std::terminate. This should only be used in unittests where termination on exception is okay.
 */
template <typename F>
class UnsafeScopeGuard {
public:
    UnsafeScopeGuard(F fun) : _fun(fun) {}

    ~UnsafeScopeGuard() noexcept(false) {
        _fun();
    }

private:
    F _fun;
};

template <typename F>
UnsafeScopeGuard<F> MakeUnsafeScopeGuard(F fun) {
    return UnsafeScopeGuard<F>(std::move(fun));
}

// Attempting to read the featureCompatibilityVersion parameter before it is explicitly initialized
// with a meaningful value will trigger failures as of SERVER-32630.
void setUpFCV() {
    serverGlobalParams.featureCompatibility.setVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44);
}
void tearDownFCV() {
    serverGlobalParams.featureCompatibility.reset();
}

struct TestSuiteEnvironment {
    explicit TestSuiteEnvironment() {
        setUpFCV();
    }

    ~TestSuiteEnvironment() noexcept(false) {
        tearDownFCV();
    }
};

struct UnitTestEnvironment {
    explicit UnitTestEnvironment(Test* const t) : test(t) {
        test->setUp();
    }

    ~UnitTestEnvironment() noexcept(false) {
        test->tearDown();
    }

    Test* const test;
};

class CaptureLogs {
public:
    ~CaptureLogs() {
        stopCapturingLogMessagesIfNeeded();
    }
    void startCapturingLogMessages();
    void stopCapturingLogMessages();
    void stopCapturingLogMessagesIfNeeded();
    const std::vector<std::string>& getCapturedTextFormatLogMessages() const;
    const std::vector<BSONObj> getCapturedBSONFormatLogMessages() const;
    int64_t countTextFormatLogLinesContaining(const std::string& needle);
    int64_t countBSONFormatLogLinesIsSubset(const BSONObj needle);
    void printCapturedTextFormatLogLines() const;

private:
    bool _isCapturingLogMessages{false};

    // Captures Plain Text Log
    std::vector<std::string> _capturedLogMessages;

    // Captured BSON
    std::vector<std::string> _capturedBSONLogMessages;
    logger::MessageLogDomain::AppenderHandle _captureAppenderHandle;
    std::unique_ptr<logger::MessageLogDomain::EventAppender> _captureAppender;

    // Capture Sink for Plain Text
    boost::shared_ptr<boost::log::sinks::synchronous_sink<logv2::LogCaptureBackend>> _captureSink;

    // Capture Sink for BSON
    boost::shared_ptr<boost::log::sinks::synchronous_sink<logv2::LogCaptureBackend>>
        _captureBSONSink;
};

static CaptureLogs* getCaptureLogs() {
    static CaptureLogs* captureLogs = new CaptureLogs();
    return captureLogs;
}

}  // namespace


Test::Test() {}

Test::~Test() {
    getCaptureLogs()->stopCapturingLogMessagesIfNeeded();
}

void Test::run() {
    UnitTestEnvironment environment(this);

    // An uncaught exception does not prevent the tear down from running. But
    // such an event still constitutes an error. To test this behavior we use a
    // special exception here that when thrown does trigger the tear down but is
    // not considered an error.
    try {
        _doTest();
    } catch (const FixtureExceptionForTesting&) {
        return;
    }
}

namespace {
class StringVectorAppender : public logger::MessageLogDomain::EventAppender {
public:
    explicit StringVectorAppender(std::vector<std::string>* lines) : _lines(lines) {}
    virtual ~StringVectorAppender() {}
    virtual Status append(const logger::MessageLogDomain::Event& event) {
        std::ostringstream _os;
        if (!_encoder.encode(event, _os)) {
            return Status(ErrorCodes::LogWriteFailed, "Failed to append to LogTestAppender.");
        }
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (_enabled) {
            _lines->push_back(_os.str());
        }
        return Status::OK();
    }

    void enable() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        invariant(!_enabled);
        _enabled = true;
    }

    void disable() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        invariant(_enabled);
        _enabled = false;
    }

private:
    stdx::mutex _mutex;  // NOLINT
    bool _enabled = false;
    logger::MessageEventDetailsEncoder _encoder;
    std::vector<std::string>* _lines;
};

void CaptureLogs::startCapturingLogMessages() {
    invariant(!_isCapturingLogMessages);
    _capturedLogMessages.clear();
    _capturedBSONLogMessages.clear();

    if (logV2Enabled()) {
        if (!_captureSink) {
            _captureSink = logv2::LogCaptureBackend::create(_capturedLogMessages);
            _captureSink->set_filter(
                logv2::AllLogsFilter(logv2::LogManager::global().getGlobalDomain()));
            _captureSink->set_formatter(logv2::PlainFormatter());

            _captureBSONSink = logv2::LogCaptureBackend::create(_capturedBSONLogMessages);

            _captureBSONSink->set_filter(
                logv2::AllLogsFilter(logv2::LogManager::global().getGlobalDomain()));
            _captureBSONSink->set_formatter(logv2::BSONFormatter());
        }
        boost::log::core::get()->add_sink(_captureSink);
        boost::log::core::get()->add_sink(_captureBSONSink);
    } else {
        if (!_captureAppender) {
            _captureAppender = std::make_unique<StringVectorAppender>(&_capturedLogMessages);
        }
        checked_cast<StringVectorAppender*>(_captureAppender.get())->enable();
        _captureAppenderHandle =
            logger::globalLogDomain()->attachAppender(std::move(_captureAppender));
    }

    _isCapturingLogMessages = true;
}

void CaptureLogs::stopCapturingLogMessages() {
    invariant(_isCapturingLogMessages);
    if (logV2Enabled()) {
        boost::log::core::get()->remove_sink(_captureSink);
        boost::log::core::get()->remove_sink(_captureBSONSink);
    } else {
        invariant(!_captureAppender);
        _captureAppender = logger::globalLogDomain()->detachAppender(_captureAppenderHandle);
        checked_cast<StringVectorAppender*>(_captureAppender.get())->disable();
    }

    _isCapturingLogMessages = false;
}

void CaptureLogs::stopCapturingLogMessagesIfNeeded() {
    if (_isCapturingLogMessages) {
        stopCapturingLogMessages();
    }
}

const std::vector<std::string>& CaptureLogs::getCapturedTextFormatLogMessages() const {
    return _capturedLogMessages;
}

const std::vector<BSONObj> CaptureLogs::getCapturedBSONFormatLogMessages() const {
    std::vector<BSONObj> objs;
    std::transform(_capturedBSONLogMessages.cbegin(),
                   _capturedBSONLogMessages.cend(),
                   std::back_inserter(objs),
                   [](const std::string& str) { return BSONObj(str.c_str()); });
    return objs;
}
void CaptureLogs::printCapturedTextFormatLogLines() const {
    LOGV2(23054,
          "****************************** Captured Lines (start) *****************************");
    for (const auto& line : getCapturedTextFormatLogMessages()) {
        LOGV2(23055, "{line}", "line"_attr = line);
    }
    LOGV2(23056,
          "****************************** Captured Lines (end) ******************************");
}

int64_t CaptureLogs::countTextFormatLogLinesContaining(const std::string& needle) {
    const auto& msgs = getCapturedTextFormatLogMessages();
    return std::count_if(
        msgs.begin(), msgs.end(), [&](const std::string& s) { return stringContains(s, needle); });
}

bool isSubset(BSONObj haystack, BSONObj needle) {
    for (const auto& element : needle) {
        auto foundElement = haystack[element.fieldNameStringData()];
        if (foundElement.eoo()) {
            return false;
        }

        // Only validate if an element exists if it is marked as undefined.
        if (element.type() == Undefined) {
            continue;
        }

        if (foundElement.canonicalType() != element.canonicalType()) {
            return false;
        }

        switch (element.type()) {
            case Object:
                if (!isSubset(foundElement.Obj(), element.Obj())) {
                    return false;
                }
                return true;
            case Array:
                // not supported
                invariant(false);
            default:
                if (SimpleBSONElementComparator::kInstance.compare(foundElement, element) != 0) {
                    return false;
                }
        }
    }

    return true;
}

int64_t CaptureLogs::countBSONFormatLogLinesIsSubset(const BSONObj needle) {
    const auto& msgs = getCapturedBSONFormatLogMessages();
    return std::count_if(
        msgs.begin(), msgs.end(), [&](const BSONObj s) { return isSubset(s, needle); });
}

}  // namespace

void Test::startCapturingLogMessages() {
    getCaptureLogs()->startCapturingLogMessages();
}
void Test::stopCapturingLogMessages() {
    getCaptureLogs()->stopCapturingLogMessages();
}
const std::vector<std::string>& Test::getCapturedTextFormatLogMessages() const {
    return getCaptureLogs()->getCapturedTextFormatLogMessages();
}
const std::vector<BSONObj> Test::getCapturedBSONFormatLogMessages() const {
    return getCaptureLogs()->getCapturedBSONFormatLogMessages();
}
int64_t Test::countTextFormatLogLinesContaining(const std::string& needle) {
    return getCaptureLogs()->countTextFormatLogLinesContaining(needle);
}
int64_t Test::countBSONFormatLogLinesIsSubset(const BSONObj needle) {
    return getCaptureLogs()->countBSONFormatLogLinesIsSubset(needle);
}
void Test::printCapturedTextFormatLogLines() const {
    getCaptureLogs()->printCapturedTextFormatLogLines();
}

Suite::Suite(ConstructorEnable, std::string name) : _name(std::move(name)) {}

void Suite::add(std::string name, std::string fileName, std::function<void()> testFn) {
    _tests.push_back({std::move(name), std::move(fileName), std::move(testFn)});
}

std::unique_ptr<Result> Suite::run(const std::string& filter,
                                   const std::string& fileNameFilter,
                                   int runsPerTest) {
    Timer timer;
    auto r = std::make_unique<Result>(_name);

    for (const auto& tc : _tests) {
        if (filter.size() && tc.name.find(filter) == std::string::npos) {
            LOGV2_DEBUG(23057,
                        1,
                        "\t skipping test: {tc_name} because it doesn't match filter",
                        "tc_name"_attr = tc.name);
            continue;
        }

        if (fileNameFilter.size() && tc.fileName.find(fileNameFilter) == std::string::npos) {
            LOGV2_DEBUG(23058,
                        1,
                        "\t skipping test: {tc_fileName} because it doesn't match fileNameFilter",
                        "tc_fileName"_attr = tc.fileName);
            continue;
        }

        ++r->_tests;

        bool passes = false;

        std::ostringstream err;
        err << tc.name << "\t";

        try {
            for (int x = 0; x < runsPerTest; x++) {
                std::ostringstream runTimes;
                if (runsPerTest > 1) {
                    runTimes << "  (" << x + 1 << "/" << runsPerTest << ")";
                }

                LOGV2(23059,
                      "\t going to run test: {tc_name}{runTimes_str}",
                      "tc_name"_attr = tc.name,
                      "runTimes_str"_attr = runTimes.str());
                TestSuiteEnvironment environment;
                tc.fn();
            }
            passes = true;
        } catch (const TestAssertionFailureException& ae) {
            err << ae.toString() << " in test " << tc.name << '\n' << ae.getStacktrace();
        } catch (const DBException& e) {
            err << "DBException: " << e.toString() << " in test " << tc.name;
        } catch (const std::exception& e) {
            err << "std::exception: " << e.what() << " in test " << tc.name;
        } catch (int x) {
            err << "caught int " << x << " in test " << tc.name;
        }

        if (!passes) {
            std::string s = err.str();
            // Don't truncate failure messages, e.g: stacktraces.
            log().setIsTruncatable(false) << "FAIL: " << s;
            r->_fails.push_back(tc.name);
            r->_messages.push_back(s);
        }
    }

    if (!r->_fails.empty())
        r->_rc = 17;

    r->_millis = timer.millis();

    LOGV2(23060, "\t DONE running tests");

    return r;
}

int Suite::run(const std::vector<std::string>& suites,
               const std::string& filter,
               const std::string& fileNameFilter,
               int runsPerTest) {
    if (suitesMap().empty()) {
        LOGV2(23061, "error: no suites registered.");
        return EXIT_FAILURE;
    }

    for (unsigned int i = 0; i < suites.size(); i++) {
        if (suitesMap().count(suites[i]) == 0) {
            LOGV2(23062,
                  "invalid test suite [{suites_i}], use --list to see valid names",
                  "suites_i"_attr = suites[i]);
            return EXIT_FAILURE;
        }
    }

    std::vector<std::string> torun(suites);

    if (torun.empty()) {
        for (const auto& kv : suitesMap()) {
            torun.push_back(std::string{kv.first});
        }
    }

    std::vector<std::unique_ptr<Result>> results;

    for (std::string name : torun) {
        std::shared_ptr<Suite>& s = suitesMap()[name];
        fassert(16145, s != nullptr);

        LOGV2(23063, "going to run suite: {name}", "name"_attr = name);
        results.push_back(s->run(filter, fileNameFilter, runsPerTest));
    }

    LOGV2(23064, "**************************************************");

    int rc = 0;

    int tests = 0;
    int asserts = 0;
    int millis = 0;

    Result totals("TOTALS");
    std::vector<std::string> failedSuites;

    for (const auto& r : results) {
        log().setIsTruncatable(false) << r->toString();
        if (abs(r->rc()) > abs(rc))
            rc = r->rc();

        tests += r->_tests;
        if (!r->_fails.empty()) {
            failedSuites.push_back(r->toString());
            for (const std::string& s : r->_fails) {
                totals._fails.push_back(r->_name + "/" + s);
            }
        }
        asserts += r->_asserts;
        millis += r->_millis;
    }
    results.clear();

    totals._tests = tests;
    totals._asserts = asserts;
    totals._millis = millis;

    LOGV2(23065, "{totals}", "totals"_attr = totals.toString());

    // summary
    if (!totals._fails.empty()) {
        LOGV2(23066, "Failing tests:");
        for (const std::string& s : totals._fails) {
            LOGV2(23067, "\t {s} Failed", "s"_attr = s);
        }
        LOGV2(23068,
              "FAILURE - {totals_fails_size} tests in {failedSuites_size} suites failed",
              "totals_fails_size"_attr = totals._fails.size(),
              "failedSuites_size"_attr = failedSuites.size());
    } else {
        LOGV2(23069, "SUCCESS - All tests in all suites passed");
    }

    return rc;
}

Suite& Suite::getSuite(StringData name) {
    auto& map = suitesMap();
    if (auto found = map.find(name); found != map.end()) {
        return *found->second;
    }
    auto sp = std::make_shared<Suite>(ConstructorEnable{}, std::string{name});
    auto [it, noCollision] = map.try_emplace(sp->key(), sp->shared_from_this());
    fassert(10162, noCollision);
    return *sp;
}

TestAssertionFailureException::TestAssertionFailureException(
    const std::string& theFile, unsigned theLine, const std::string& theFailingExpression)
    : _file(theFile), _line(theLine), _message(theFailingExpression) {
    std::ostringstream ostream;
    printStackTrace(ostream);
    _stacktrace = ostream.str();
}

std::string TestAssertionFailureException::toString() const {
    std::ostringstream os;
    os << getMessage() << " @" << getFile() << ":" << getLine();
    return os.str();
}

TestAssertionFailure::TestAssertionFailure(const std::string& file,
                                           unsigned line,
                                           const std::string& message)
    : _exception(file, line, message), _enabled(false) {}

TestAssertionFailure::TestAssertionFailure(const TestAssertionFailure& other)
    : _exception(other._exception), _enabled(false) {
    invariant(!other._enabled);
}

TestAssertionFailure& TestAssertionFailure::operator=(const TestAssertionFailure& other) {
    invariant(!_enabled);
    invariant(!other._enabled);
    _exception = other._exception;
    return *this;
}

TestAssertionFailure::~TestAssertionFailure() noexcept(false) {
    if (!_enabled) {
        invariant(_stream.str().empty());
        return;
    }
    if (!_stream.str().empty()) {
        _exception.setMessage(_exception.getMessage() + " " + _stream.str());
    }
    LOGV2_ERROR(23070, "Throwing exception: {exception}", "exception"_attr = _exception);
    throw _exception;
}

std::ostream& TestAssertionFailure::stream() {
    invariant(!_enabled);
    _enabled = true;
    return _stream;
}

std::vector<std::string> getAllSuiteNames() {
    std::vector<std::string> result;
    for (const auto& kv : suitesMap()) {
        result.push_back(std::string{kv.first});
    }
    return result;
}

template <ComparisonOp op>
ComparisonAssertion<op> ComparisonAssertion<op>::make(const char* theFile,
                                                      unsigned theLine,
                                                      StringData aExpression,
                                                      StringData bExpression,
                                                      StringData a,
                                                      StringData b) {
    return ComparisonAssertion(theFile, theLine, aExpression, bExpression, a, b);
}

template <ComparisonOp op>
ComparisonAssertion<op> ComparisonAssertion<op>::make(const char* theFile,
                                                      unsigned theLine,
                                                      StringData aExpression,
                                                      StringData bExpression,
                                                      const void* a,
                                                      const void* b) {
    return ComparisonAssertion(theFile, theLine, aExpression, bExpression, a, b);
}

// Provide definitions for common instantiations of ComparisonAssertion.
INSTANTIATE_COMPARISON_ASSERTION_CTORS();

}  // namespace unittest
}  // namespace mongo
