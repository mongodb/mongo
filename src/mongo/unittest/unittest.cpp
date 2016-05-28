/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/unittest/unittest.h"

#include <iostream>
#include <map>

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/logger/console_appender.h"
#include "mongo/logger/log_manager.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/logger/message_log_domain.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::shared_ptr;
using std::string;

namespace unittest {

namespace {

bool stringContains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

logger::MessageLogDomain* unittestOutput = logger::globalLogManager()->getNamedDomain("unittest");

typedef std::map<std::string, std::shared_ptr<Suite>> SuiteMap;

inline SuiteMap& _allSuites() {
    static SuiteMap allSuites;
    return allSuites;
}

}  // namespace

logger::LogstreamBuilder log() {
    return LogstreamBuilder(unittestOutput, getThreadName(), logger::LogSeverity::Log());
}

MONGO_INITIALIZER_WITH_PREREQUISITES(UnitTestOutput, ("GlobalLogManager", "default"))
(InitializerContext*) {
    unittestOutput->attachAppender(logger::MessageLogDomain::AppenderAutoPtr(
        new logger::ConsoleAppender<logger::MessageLogDomain::Event>(
            new logger::MessageEventDetailsEncoder)));
    return Status::OK();
}

class Result {
public:
    Result(const std::string& name)
        : _name(name), _rc(0), _tests(0), _fails(), _asserts(0), _millis(0) {}

    std::string toString() {
        std::stringstream ss;

        char result[128];
        sprintf(result,
                "%-30s | tests: %4d | fails: %4d | assert calls: %10d | time secs: %6.3f\n",
                _name.c_str(),
                _tests,
                static_cast<int>(_fails.size()),
                _asserts,
                _millis / 1000.0);
        ss << result;

        for (std::vector<std::string>::iterator i = _messages.begin(); i != _messages.end(); i++) {
            ss << "\t" << *i << '\n';
        }

        return ss.str();
    }

    int rc() {
        return _rc;
    }

    string _name;

    int _rc;
    int _tests;
    std::vector<std::string> _fails;
    int _asserts;
    int _millis;
    std::vector<std::string> _messages;

    static Result* cur;
};

Result* Result::cur = 0;

Test::Test() : _isCapturingLogMessages(false) {}

Test::~Test() {
    if (_isCapturingLogMessages) {
        stopCapturingLogMessages();
    }
}

void Test::run() {
    setUp();

    // An uncaught exception does not prevent the tear down from running. But
    // such an event still constitutes an error. To test this behavior we use a
    // special exception here that when thrown does trigger the tear down but is
    // not considered an error.
    try {
        _doTest();
    } catch (FixtureExceptionForTesting&) {
        tearDown();
        return;
    } catch (TestAssertionFailureException&) {
        tearDown();
        throw;
    }

    tearDown();
}

void Test::setUp() {}
void Test::tearDown() {}

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
    stdx::mutex _mutex;
    bool _enabled = false;
    logger::MessageEventDetailsEncoder _encoder;
    std::vector<std::string>* _lines;
};
}  // namespace

void Test::startCapturingLogMessages() {
    invariant(!_isCapturingLogMessages);
    _capturedLogMessages.clear();
    if (!_captureAppender) {
        _captureAppender = stdx::make_unique<StringVectorAppender>(&_capturedLogMessages);
    }
    checked_cast<StringVectorAppender*>(_captureAppender.get())->enable();
    _captureAppenderHandle = logger::globalLogDomain()->attachAppender(std::move(_captureAppender));
    _isCapturingLogMessages = true;
}

void Test::stopCapturingLogMessages() {
    invariant(_isCapturingLogMessages);
    invariant(!_captureAppender);
    _captureAppender = logger::globalLogDomain()->detachAppender(_captureAppenderHandle);
    checked_cast<StringVectorAppender*>(_captureAppender.get())->disable();
    _isCapturingLogMessages = false;
}
void Test::printCapturedLogLines() const {
    log() << "****************************** Captured Lines (start) *****************************";
    std::for_each(getCapturedLogMessages().begin(),
                  getCapturedLogMessages().end(),
                  [](std::string line) { log() << line; });
    log() << "****************************** Captured Lines (end) ******************************";
}

int64_t Test::countLogLinesContaining(const std::string& needle) {
    return std::count_if(getCapturedLogMessages().begin(),
                         getCapturedLogMessages().end(),
                         stdx::bind(stringContains, stdx::placeholders::_1, needle));
}

Suite::Suite(const std::string& name) : _name(name) {
    registerSuite(name, this);
}

Suite::~Suite() {}

void Suite::add(const std::string& name, const TestFunction& testFn) {
    _tests.push_back(std::shared_ptr<TestHolder>(new TestHolder(name, testFn)));
}

Result* Suite::run(const std::string& filter, int runsPerTest) {
    LOG(1) << "\t about to setupTests" << std::endl;
    setupTests();
    LOG(1) << "\t done setupTests" << std::endl;

    Timer timer;
    Result* r = new Result(_name);
    Result::cur = r;

    for (std::vector<std::shared_ptr<TestHolder>>::iterator i = _tests.begin(); i != _tests.end();
         i++) {
        std::shared_ptr<TestHolder>& tc = *i;
        if (filter.size() && tc->getName().find(filter) == std::string::npos) {
            LOG(1) << "\t skipping test: " << tc->getName() << " because doesn't match filter"
                   << std::endl;
            continue;
        }

        r->_tests++;

        bool passes = false;

        log() << "\t going to run test: " << tc->getName() << std::endl;

        std::stringstream err;
        err << tc->getName() << "\t";

        try {
            for (int x = 0; x < runsPerTest; x++)
                tc->run();
            passes = true;
        } catch (const TestAssertionFailureException& ae) {
            err << ae.toString();
        } catch (const std::exception& e) {
            err << " std::exception: " << e.what() << " in test " << tc->getName();
        } catch (int x) {
            err << " caught int " << x << " in test " << tc->getName();
        }

        if (!passes) {
            std::string s = err.str();
            log() << "FAIL: " << s << std::endl;
            r->_fails.push_back(tc->getName());
            r->_messages.push_back(s);
        }
    }

    if (!r->_fails.empty())
        r->_rc = 17;

    r->_millis = timer.millis();

    log() << "\t DONE running tests" << std::endl;

    return r;
}

int Suite::run(const std::vector<std::string>& suites, const std::string& filter, int runsPerTest) {
    if (_allSuites().empty()) {
        log() << "error: no suites registered.";
        return EXIT_FAILURE;
    }

    for (unsigned int i = 0; i < suites.size(); i++) {
        if (_allSuites().count(suites[i]) == 0) {
            log() << "invalid test suite [" << suites[i] << "], use --list to see valid names"
                  << std::endl;
            return EXIT_FAILURE;
        }
    }

    std::vector<std::string> torun(suites);

    if (torun.empty()) {
        for (SuiteMap::const_iterator i = _allSuites().begin(); i != _allSuites().end(); ++i) {
            torun.push_back(i->first);
        }
    }

    std::vector<Result*> results;

    for (std::vector<std::string>::iterator i = torun.begin(); i != torun.end(); i++) {
        std::string name = *i;
        std::shared_ptr<Suite>& s = _allSuites()[name];
        fassert(16145, s != NULL);

        log() << "going to run suite: " << name << std::endl;
        results.push_back(s->run(filter, runsPerTest));
    }

    log() << "**************************************************" << std::endl;

    int rc = 0;

    int tests = 0;
    int asserts = 0;
    int millis = 0;

    Result totals("TOTALS");
    std::vector<std::string> failedSuites;

    Result::cur = NULL;
    for (std::vector<Result*>::iterator i = results.begin(); i != results.end(); i++) {
        Result* r = *i;
        log() << r->toString();
        if (abs(r->rc()) > abs(rc))
            rc = r->rc();

        tests += r->_tests;
        if (!r->_fails.empty()) {
            failedSuites.push_back(r->toString());
            for (std::vector<std::string>::const_iterator j = r->_fails.begin();
                 j != r->_fails.end();
                 j++) {
                const std::string& s = (*j);
                totals._fails.push_back(r->_name + "/" + s);
            }
        }
        asserts += r->_asserts;
        millis += r->_millis;

        delete r;
    }

    totals._tests = tests;
    totals._asserts = asserts;
    totals._millis = millis;

    log() << totals.toString();  // includes endl

    // summary
    if (!totals._fails.empty()) {
        log() << "Failing tests:" << std::endl;
        for (std::vector<std::string>::const_iterator i = totals._fails.begin();
             i != totals._fails.end();
             i++) {
            const std::string& s = (*i);
            log() << "\t " << s << " Failed";
        }
        log() << "FAILURE - " << totals._fails.size() << " tests in " << failedSuites.size()
              << " suites failed";
    } else {
        log() << "SUCCESS - All tests in all suites passed";
    }

    return rc;
}

void Suite::registerSuite(const std::string& name, Suite* s) {
    std::shared_ptr<Suite>& m = _allSuites()[name];
    fassert(10162, !m);
    m.reset(s);
}

Suite* Suite::getSuite(const std::string& name) {
    std::shared_ptr<Suite>& result = _allSuites()[name];
    if (!result) {
        // Suites are self-registering.
        new Suite(name);
    }
    invariant(result);
    return result.get();
}

void Suite::setupTests() {}

TestAssertionFailureException::TestAssertionFailureException(
    const std::string& theFile, unsigned theLine, const std::string& theFailingExpression)
    : _file(theFile), _line(theLine), _message(theFailingExpression) {}

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

TestAssertionFailure::~TestAssertionFailure() BOOST_NOEXCEPT_IF(false) {
    if (!_enabled) {
        invariant(_stream.str().empty());
        return;
    }
    if (!_stream.str().empty()) {
        _exception.setMessage(_exception.getMessage() + " " + _stream.str());
    }
    throw _exception;
}

std::ostream& TestAssertionFailure::stream() {
    invariant(!_enabled);
    _enabled = true;
    return _stream;
}

std::vector<std::string> getAllSuiteNames() {
    std::vector<std::string> result;
    for (SuiteMap::const_iterator i = _allSuites().begin(); i != _allSuites().end(); ++i) {
        result.push_back(i->first);
    }
    return result;
}

}  // namespace unittest
}  // namespace mongo
