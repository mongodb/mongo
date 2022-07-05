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
#include "mongo/logv2/bson_formatter.h"
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_capture_backend.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/log_truncation.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/pcre.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/timer.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::unittest {
namespace {

bool stringContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/** Each map key is owned by its corresponding Suite object. */
auto& suitesMap() {
    static std::map<StringData, std::shared_ptr<Suite>> m;
    return m;
}

}  // namespace

bool searchRegex(const std::string& pattern, const std::string& string) {
    return !!pcre::Regex(pattern).matchView(string);
}

class Result {
public:
    struct FailStatus {
        std::string test;
        std::string type;
        std::string error;
        std::string extra;

        friend std::ostream& operator<<(std::ostream& os, const FailStatus& f) {
            return os                      //
                << "{test: " << f.test     //
                << ", type: " << f.type    //
                << ", error: " << f.error  //
                << ", extra: " << f.extra  //
                << "}";
        }
    };

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

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        bob.append("name", _name);
        bob.append("tests", _tests);
        bob.appendNumber("fails", static_cast<long long>(_fails.size()));
        bob.append("asserts", _asserts);
        bob.append("time", Milliseconds(_millis).toBSON());
        {
            auto arr = BSONArrayBuilder(bob.subarrayStart("failures"));
            for (const auto& m : _messages) {
                auto o = BSONObjBuilder(arr.subobjStart());
                o.append("test", m.test);
                o.append("type", m.type);
                o.append("error", m.error);
                if (!m.extra.empty()) {
                    o.append("extra", m.extra);
                }
            }
        }
        return bob.obj();
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
    std::vector<FailStatus> _messages;
};

namespace {

// Attempting to read the featureCompatibilityVersion parameter before it is explicitly initialized
// with a meaningful value will trigger failures as of SERVER-32630.
// (Generic FCV reference): This FCV reference should exist across LTS binary versions.
void setUpFCV() {
    serverGlobalParams.mutableFeatureCompatibility.setVersion(multiversion::GenericFCV::kLatest);
}
void tearDownFCV() {
    serverGlobalParams.mutableFeatureCompatibility.reset();
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
    std::vector<BSONObj> getCapturedBSONFormatLogMessages() const;
    int64_t countTextFormatLogLinesContaining(const std::string& needle);
    int64_t countBSONFormatLogLinesIsSubset(const BSONObj& needle);
    void printCapturedTextFormatLogLines() const;

private:
    bool _isCapturingLogMessages{false};

    // Captures Plain Text Log
    std::vector<std::string> _capturedLogMessages;

    // Captured BSON
    std::vector<std::string> _capturedBSONLogMessages;

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

void CaptureLogs::startCapturingLogMessages() {
    invariant(!_isCapturingLogMessages);
    _capturedLogMessages.clear();
    _capturedBSONLogMessages.clear();

    if (!_captureSink) {
        _captureSink = logv2::LogCaptureBackend::create(_capturedLogMessages, true);
        _captureSink->set_filter(
            logv2::AllLogsFilter(logv2::LogManager::global().getGlobalDomain()));
        _captureSink->set_formatter(logv2::PlainFormatter());

        _captureBSONSink = logv2::LogCaptureBackend::create(_capturedBSONLogMessages, false);

        _captureBSONSink->set_filter(
            logv2::AllLogsFilter(logv2::LogManager::global().getGlobalDomain()));
        _captureBSONSink->set_formatter(logv2::BSONFormatter());
    }
    boost::log::core::get()->add_sink(_captureSink);
    boost::log::core::get()->add_sink(_captureBSONSink);

    _isCapturingLogMessages = true;
}

void CaptureLogs::stopCapturingLogMessages() {
    invariant(_isCapturingLogMessages);
    boost::log::core::get()->remove_sink(_captureSink);
    boost::log::core::get()->remove_sink(_captureBSONSink);

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

std::vector<BSONObj> CaptureLogs::getCapturedBSONFormatLogMessages() const {
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
                // This annotation shouldn't really be needed because
                // `invariantFailed` is annotated to be `noreturn`,
                // but clang 12 doesn't seem to be able to capitalize
                // on that fact to see that we are not actually
                // falling through.
                [[fallthrough]];
            default:
                if (SimpleBSONElementComparator::kInstance.compare(foundElement, element) != 0) {
                    return false;
                }
        }
    }

    return true;
}

int64_t CaptureLogs::countBSONFormatLogLinesIsSubset(const BSONObj& needle) {
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
std::vector<BSONObj> Test::getCapturedBSONFormatLogMessages() const {
    return getCaptureLogs()->getCapturedBSONFormatLogMessages();
}
int64_t Test::countTextFormatLogLinesContaining(const std::string& needle) {
    return getCaptureLogs()->countTextFormatLogLinesContaining(needle);
}
int64_t Test::countBSONFormatLogLinesIsSubset(const BSONObj& needle) {
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

    boost::optional<pcre::Regex> filterRe;
    boost::optional<pcre::Regex> fileNameFilterRe;
    if (!filter.empty())
        filterRe.emplace(filter);
    if (!fileNameFilter.empty())
        fileNameFilterRe.emplace(fileNameFilter);

    for (const auto& tc : _tests) {
        if (filterRe && !filterRe->matchView(tc.name)) {
            LOGV2_DEBUG(23057, 3, "skipped due to filter", "test"_attr = tc.name);
            continue;
        }

        if (fileNameFilterRe && !fileNameFilterRe->matchView(tc.fileName)) {
            LOGV2_DEBUG(23058, 3, "skipped due to fileNameFilter", "testFile"_attr = tc.fileName);
            continue;
        }

        // This test hasn't been skipped, and is about to run. If it's the first one in this suite
        // (ie. _tests is zero), then output the suite header before running it.
        if (r->_tests == 0) {
            LOGV2(23063, "Running", "suite"_attr = _name);
        }
        ++r->_tests;

        struct Event {
            std::string type;
            std::string error;
            std::string extra;
        };
        try {
            try {
                for (int x = 0; x < runsPerTest; x++) {
                    LOGV2(23059,
                          "Running",
                          "test"_attr = tc.name,
                          "rep"_attr = x + 1,
                          "reps"_attr = runsPerTest);
                    TestSuiteEnvironment environment;
                    tc.fn();
                }
            } catch (const TestAssertionFailureException& ae) {
                throw Event{"TestAssertionFailureException", ae.toString(), ae.getStacktrace()};
            } catch (const DBException& e) {
                throw Event{"DBException", e.toString()};
            } catch (const std::exception& e) {
                throw Event{"std::exception", e.what()};
            } catch (int x) {
                throw Event{"int", std::to_string(x)};
            }
        } catch (const Event& e) {
            LOGV2_OPTIONS(4680100,
                          {logv2::LogTruncation::Disabled},
                          "FAIL",
                          "test"_attr = tc.name,
                          "type"_attr = e.type,
                          "error"_attr = e.error,
                          "extra"_attr = e.extra);
            r->_fails.push_back(tc.name);
            r->_messages.push_back({tc.name, e.type, e.error, e.extra});
        }
    }

    if (!r->_fails.empty())
        r->_rc = 17;

    r->_millis = timer.millis();

    // Only show the footer if some tests were run in this suite.
    if (r->_tests > 0) {
        LOGV2(23060, "Done running tests");
    }

    return r;
}

int Suite::run(const std::vector<std::string>& suites,
               const std::string& filter,
               const std::string& fileNameFilter,
               int runsPerTest) {
    if (suitesMap().empty()) {
        LOGV2_ERROR(23061, "no suites registered.");
        return static_cast<int>(ExitCode::fail);
    }

    for (unsigned int i = 0; i < suites.size(); i++) {
        if (suitesMap().count(suites[i]) == 0) {
            LOGV2_ERROR(23062,
                        "invalid test suite, use --list to see valid names",
                        "suite"_attr = suites[i]);
            return static_cast<int>(ExitCode::fail);
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

        auto result = s->run(filter, fileNameFilter, runsPerTest);
        results.push_back(std::move(result));
    }

    int rc = 0;

    int tests = 0;
    int asserts = 0;
    int millis = 0;

    Result totals("TOTALS");
    std::vector<BSONObj> failedSuites;

    for (const auto& r : results) {
        if (abs(r->rc()) > abs(rc))
            rc = r->rc();

        tests += r->_tests;
        if (!r->_fails.empty()) {
            failedSuites.push_back(r->toBSON());
            for (const std::string& failedTest : r->_fails) {
                totals._fails.push_back(r->_name + "/" + failedTest);
            }
        }
        asserts += r->_asserts;
        millis += r->_millis;
    }
    totals._tests = tests;
    totals._asserts = asserts;
    totals._millis = millis;

    for (const auto& r : results) {
        // Only show results from a suite if some tests were run in it.
        if (r->_tests > 0) {
            LOGV2_OPTIONS(
                4680101, {logv2::LogTruncation::Disabled}, "Result", "suite"_attr = r->toBSON());
        }
    }
    LOGV2(23065, "Totals", "totals"_attr = totals.toBSON());

    // summary
    if (!totals._fails.empty()) {
        LOGV2_OPTIONS(23068,
                      {logv2::LogTruncation::Disabled},
                      "FAILURE",
                      "failedTestsCount"_attr = totals._fails.size(),
                      "failedSuitesCount"_attr = failedSuites.size(),
                      "failedTests"_attr = totals._fails);
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

TestAssertionFailureException::TestAssertionFailureException(std::string file,
                                                             unsigned line,
                                                             std::string message)
    : _file(std::move(file)), _line(line), _message(std::move(message)) {
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
    LOGV2_ERROR(23070, "Throwing exception", "exception"_attr = _exception);
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

UnitTest* UnitTest::getInstance() {
    static auto p = new UnitTest;
    return p;
}

const TestInfo* UnitTest::currentTestInfo() const {
    return _currentTestInfo;
}

void UnitTest::setCurrentTestInfo(const TestInfo* testInfo) {
    _currentTestInfo = testInfo;
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


SpawnInfo& getSpawnInfo() {
    static auto v = new SpawnInfo{};
    return *v;
}

namespace {
// At startup, teach the terminate handler how to print TestAssertionFailureException.
[[maybe_unused]] const auto earlyCall = [] {
    globalActiveExceptionWitness().addHandler<TestAssertionFailureException>(
        [](const auto& ex, std::ostream& os) { os << ex.toString() << "\n"; });
    return 0;
}();
}  // namespace

}  // namespace mongo::unittest
