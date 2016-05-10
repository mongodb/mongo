/*    Copyright 2013 10gen Inc.
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

#include "mongo/logger/log_test.h"

#include <sstream>
#include <string>
#include <vector>

#include "mongo/logger/appender.h"
#include "mongo/logger/encoder.h"
#include "mongo/logger/log_component.h"
#include "mongo/logger/log_component_settings.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/logger/message_log_domain.h"
#include "mongo/logger/rotatable_file_appender.h"
#include "mongo/logger/rotatable_file_writer.h"
#include "mongo/platform/compiler.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

using namespace mongo::logger;

namespace mongo {
namespace {

typedef LogTest<MessageEventDetailsEncoder> LogTestDetailsEncoder;
typedef LogTest<MessageEventUnadornedEncoder> LogTestUnadornedEncoder;

TEST_F(LogTestUnadornedEncoder, logContext) {
    logContext("WHA!");
    ASSERT_GREATER_THAN(_logLines.size(), 1U);
    ASSERT_NOT_EQUALS(_logLines[0].find("WHA!"), std::string::npos);

    // TODO(schwerin): Ensure that logContext rights a proper context to the log stream,
    // including the address of the logContext() function.
    // void const* logContextFn = reinterpret_cast<void const*>(logContext);
}

class CountAppender : public Appender<MessageEventEphemeral> {
public:
    CountAppender() : _count(0) {}
    virtual ~CountAppender() {}

    virtual Status append(const MessageEventEphemeral& event) {
        ++_count;
        return Status::OK();
    }

    int getCount() {
        return _count;
    }

private:
    int _count;
};

/** Simple tests for detaching appenders. */
TEST_F(LogTestUnadornedEncoder, DetachAppender) {
    MessageLogDomain::AppenderAutoPtr countAppender(new CountAppender);
    MessageLogDomain domain;

    // Appending to the domain before attaching the appender does not affect the appender.
    domain.append(MessageEventEphemeral(Date_t(), LogSeverity::Log(), "", "1"));
    ASSERT_EQUALS(0, dynamic_cast<CountAppender*>(countAppender.get())->getCount());

    // Appending to the domain after attaching the appender does affect the appender.
    MessageLogDomain::AppenderHandle handle = domain.attachAppender(std::move(countAppender));
    domain.append(MessageEventEphemeral(Date_t(), LogSeverity::Log(), "", "2"));
    countAppender = domain.detachAppender(handle);
    ASSERT_EQUALS(1, dynamic_cast<CountAppender*>(countAppender.get())->getCount());

    // Appending to the domain after detaching the appender does not affect the appender.
    domain.append(MessageEventEphemeral(Date_t(), LogSeverity::Log(), "", "3"));
    ASSERT_EQUALS(1, dynamic_cast<CountAppender*>(countAppender.get())->getCount());
}

class A {
public:
    std::string toString() const {
        log() << "Golly!\n";
        return "Golly!";
    }
};

// Tests that logging while in the midst of logging produces two distinct log messages, with the
// inner log message appearing before the outer.
TEST_F(LogTestUnadornedEncoder, LogstreamBuilderReentrance) {
    log() << "Logging A() -- " << A() << " -- done!" << std::endl;
    ASSERT_EQUALS(2U, _logLines.size());
    ASSERT_EQUALS(std::string("Golly!\n"), _logLines[0]);
    ASSERT_EQUALS(std::string("Logging A() -- Golly! -- done!\n"), _logLines[1]);
}

//
// Instantiating this object is a basic test of static-initializer-time logging.
//
class B {
public:
    B() {
        log() << "Exercising initializer time logging.";
    }
} b;

// Constants for log component test cases.
const LogComponent componentDefault = LogComponent::kDefault;
const LogComponent componentA = LogComponent::kCommand;
const LogComponent componentB = LogComponent::kAccessControl;
const LogComponent componentC = LogComponent::kNetwork;
const LogComponent componentD = LogComponent::kStorage;
const LogComponent componentE = LogComponent::kJournal;

// No log component declared at file scope.
// Component severity configuration:
//     LogComponent::kDefault: 2
TEST_F(LogTestUnadornedEncoder, MongoLogMacroNoFileScopeLogComponent) {
    globalLogDomain()->setMinimumLoggedSeverity(LogSeverity::Debug(2));

    LOG(2) << "This is logged";
    LOG(3) << "This is not logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

    // MONGO_LOG_COMPONENT
    _logLines.clear();
    MONGO_LOG_COMPONENT(2, componentA) << "This is logged";
    MONGO_LOG_COMPONENT(3, componentA) << "This is not logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

    // MONGO_LOG_COMPONENT2
    _logLines.clear();
    MONGO_LOG_COMPONENT2(2, componentA, componentB) << "This is logged";
    MONGO_LOG_COMPONENT2(3, componentA, componentB) << "This is not logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

    // MONGO_LOG_COMPONENT3
    _logLines.clear();
    MONGO_LOG_COMPONENT3(2, componentA, componentB, componentC) << "This is logged";
    MONGO_LOG_COMPONENT3(3, componentA, componentB, componentC) << "This is not logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);
}

//
// Component log level tests.
// The global log manager holds the component log level configuration for the global log domain.
// LOG() and MONGO_LOG_COMPONENT() macros in util/log.h determine at runtime if a log message
// should be written to the log domain.
//

TEST_F(LogTestUnadornedEncoder, LogComponentSettingsMinimumLogSeverity) {
    LogComponentSettings settings;
    ASSERT_TRUE(settings.hasMinimumLogSeverity(LogComponent::kDefault));
    ASSERT_TRUE(settings.getMinimumLogSeverity(LogComponent::kDefault) == LogSeverity::Log());
    for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
        LogComponent component = static_cast<LogComponent::Value>(i);
        if (component == LogComponent::kDefault) {
            continue;
        }
        ASSERT_FALSE(settings.hasMinimumLogSeverity(component));
    }

    // Override and clear minimum severity level.
    for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
        LogComponent component = static_cast<LogComponent::Value>(i);
        LogSeverity severity = LogSeverity::Debug(2);

        // Override severity level.
        settings.setMinimumLoggedSeverity(component, severity);
        ASSERT_TRUE(settings.hasMinimumLogSeverity(component));
        ASSERT_TRUE(settings.getMinimumLogSeverity(component) == severity);

        // Clear severity level.
        // Special case: when clearing LogComponent::kDefault, the corresponding
        //               severity level is set to default values (ie. Log()).
        settings.clearMinimumLoggedSeverity(component);
        if (component == LogComponent::kDefault) {
            ASSERT_TRUE(settings.hasMinimumLogSeverity(component));
            ASSERT_TRUE(settings.getMinimumLogSeverity(LogComponent::kDefault) ==
                        LogSeverity::Log());
        } else {
            ASSERT_FALSE(settings.hasMinimumLogSeverity(component));
        }
    }
}

// Test for shouldLog() when the minimum logged severity is set only for LogComponent::kDefault.
TEST_F(LogTestUnadornedEncoder, LogComponentSettingsShouldLogDefaultLogComponentOnly) {
    LogComponentSettings settings;

    // Initial log severity for LogComponent::kDefault is Log().
    ASSERT_TRUE(shouldLog(LogSeverity::Info()));
    ASSERT_TRUE(shouldLog(LogSeverity::Log()));
    ASSERT_FALSE(shouldLog(LogSeverity::Debug(1)));
    ASSERT_FALSE(shouldLog(LogSeverity::Debug(2)));

    // If any components are provided to shouldLog(), we should get the same outcome
    // because we have not configured any non-LogComponent::kDefault components.
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Log()));
    ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(1)));

    // Set minimum logged severity so that Debug(1) messages are written to log domain.
    settings.setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Debug(1));
    logger::globalLogDomain()->setMinimumLoggedSeverity(LogComponent::kDefault,
                                                        LogSeverity::Debug(1));

    ASSERT_TRUE(shouldLog(LogSeverity::Info()));
    ASSERT_TRUE(shouldLog(LogSeverity::Log()));
    ASSERT_TRUE(shouldLog(LogSeverity::Debug(1)));
    ASSERT_FALSE(shouldLog(LogSeverity::Debug(2)));

    // Revert back.
    logger::globalLogDomain()->setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Log());

    // Same results when components are supplied to shouldLog().
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Debug(1)));
    ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(2)));
}

// Test for shouldLog() when we have configured a single component.
// Also checks that severity level has been reverted to match LogComponent::kDefault
// after clearing level.
// Minimum severity levels:
// LogComponent::kDefault: 1
// componentA: 2
TEST_F(LogTestUnadornedEncoder, LogComponentSettingsShouldLogSingleComponent) {
    LogComponentSettings settings;

    settings.setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Debug(1));
    settings.setMinimumLoggedSeverity(componentA, LogSeverity::Debug(2));

    // Components for log message: componentA only.
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Debug(2)));
    ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(3)));

    // Clear severity level for componentA and check shouldLog() again.
    settings.clearMinimumLoggedSeverity(componentA);
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Debug(1)));
    ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(2)));

    // Test shouldLog() with global settings.
    logger::globalLogDomain()->setMinimumLoggedSeverity(LogComponent::kDefault,
                                                        LogSeverity::Debug(1));

    // Components for log message: LogComponent::kDefault only.
    ASSERT_TRUE(shouldLog(LogSeverity::Debug(1)));
    ASSERT_FALSE(shouldLog(LogSeverity::Debug(2)));

    logger::globalLogDomain()->setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Log());
}

// Test for shouldLog() when we have configured multiple components.
// Minimum severity levels:
// LogComponent::kDefault: 1
// componentA: 2
// componentB: 0
TEST_F(LogTestUnadornedEncoder, LogComponentSettingsShouldLogMultipleComponentsConfigured) {
    LogComponentSettings settings;

    settings.setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Debug(1));
    settings.setMinimumLoggedSeverity(componentA, LogSeverity::Debug(2));
    settings.setMinimumLoggedSeverity(componentB, LogSeverity::Log());

    // Components for log message: componentA only.
    ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Debug(2)));
    ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(3)));

    // Components for log message: componentB only.
    ASSERT_TRUE(settings.shouldLog(componentB, LogSeverity::Log()));
    ASSERT_FALSE(settings.shouldLog(componentB, LogSeverity::Debug(1)));

    // Components for log message: componentC only.
    // Since a component-specific minimum severity is not configured for componentC,
    // shouldLog() falls back on LogComponent::kDefault.
    ASSERT_TRUE(settings.shouldLog(componentC, LogSeverity::Debug(1)));
    ASSERT_FALSE(settings.shouldLog(componentC, LogSeverity::Debug(2)));

    // Test shouldLog() with global settings.
    logger::globalLogDomain()->setMinimumLoggedSeverity(LogComponent::kDefault,
                                                        LogSeverity::Debug(1));


    // Components for log message: LogComponent::kDefault only.
    ASSERT_TRUE(shouldLog(LogSeverity::Debug(1)));
    ASSERT_FALSE(shouldLog(LogSeverity::Debug(2)));

    logger::globalLogDomain()->setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Log());
}

// Log component hierarchy.
TEST_F(LogTestUnadornedEncoder, LogComponentHierarchy) {
    // Parent component is not meaningful for kDefault and kNumLogComponents.
    ASSERT_EQUALS(LogComponent::kNumLogComponents, LogComponent(LogComponent::kDefault).parent());
    ASSERT_EQUALS(LogComponent::kNumLogComponents,
                  LogComponent(LogComponent::kNumLogComponents).parent());

    // Default -> ComponentD -> ComponentE
    ASSERT_EQUALS(LogComponent::kDefault, LogComponent(componentD).parent());
    ASSERT_EQUALS(componentD, LogComponent(componentE).parent());
    ASSERT_NOT_EQUALS(LogComponent::kDefault, LogComponent(componentE).parent());

    // Log components should inherit parent's log severity in settings.
    LogComponentSettings settings;
    settings.setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Debug(1));
    settings.setMinimumLoggedSeverity(componentD, LogSeverity::Debug(2));

    // componentE should inherit componentD's log severity.
    ASSERT_TRUE(settings.shouldLog(componentE, LogSeverity::Debug(2)));
    ASSERT_FALSE(settings.shouldLog(componentE, LogSeverity::Debug(3)));

    // Clearing parent's log severity - componentE should inherit from Default.
    settings.clearMinimumLoggedSeverity(componentD);
    ASSERT_TRUE(settings.shouldLog(componentE, LogSeverity::Debug(1)));
    ASSERT_FALSE(settings.shouldLog(componentE, LogSeverity::Debug(2)));
}

// Dotted name of component includes names of ancestors.
TEST_F(LogTestUnadornedEncoder, LogComponentDottedName) {
    // Default -> ComponentD -> ComponentE
    ASSERT_EQUALS(componentDefault.getShortName(),
                  LogComponent(LogComponent::kDefault).getDottedName());
    ASSERT_EQUALS(componentD.getShortName(), componentD.getDottedName());
    ASSERT_EQUALS(componentD.getShortName() + "." + componentE.getShortName(),
                  componentE.getDottedName());
}

// Log names of all components should have the same length.
TEST_F(LogTestUnadornedEncoder, LogComponentNameForLog) {
    size_t defaultNameForLogLength = componentDefault.getNameForLog().toString().length();
    ASSERT_NOT_EQUALS(0U, defaultNameForLogLength);
    for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
        LogComponent component = static_cast<LogComponent::Value>(i);
        ASSERT_EQUALS(defaultNameForLogLength, component.getNameForLog().toString().length());
    }
}

/**
 * Verifies that the encoded log line contains string.
 */
void testEncodedLogLine(const MessageEventEphemeral& event, const std::string& expectedSubstring) {
    MessageEventDetailsEncoder encoder;
    std::ostringstream os;
    ASSERT_TRUE(encoder.encode(event, os));
    std::string s = os.str();
    if (s.find(expectedSubstring) == std::string::npos) {
        FAIL(str::stream() << "encoded log line does not contain substring \"" << expectedSubstring
                           << "\". log line: "
                           << s);
    }
}

// Log severity should always be logged as a single capital letter.
TEST_F(LogTestUnadornedEncoder, MessageEventDetailsEncoderLogSeverity) {
    Date_t d = Date_t::now();
    const auto ctx = "WHAT"_sd;
    const auto msg = "HUH"_sd;
    // Severe is indicated by (F)atal.
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Severe(), ctx, msg), " F ");
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Error(), ctx, msg), " E ");
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Warning(), ctx, msg), " W ");
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Info(), ctx, msg), " I ");
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Log(), ctx, msg), " I ");
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Debug(0), ctx, msg), " I ");
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Debug(1), ctx, msg), " D ");
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Debug(2), ctx, msg), " D ");
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Debug(3), ctx, msg), " D ");
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Debug(4), ctx, msg), " D ");
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Debug(5), ctx, msg), " D ");
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Debug(100), ctx, msg), " D ");
    // Unknown severity.
    testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Severe().moreSevere(), ctx, msg),
                       " U ");
}

// Non-default log component short name should always be logged.
TEST_F(LogTestUnadornedEncoder, MessageEventDetailsEncoderLogComponent) {
    Date_t d = Date_t::now();
    const auto ctx = "WHAT"_sd;
    const auto msg = "HUH"_sd;
    for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
        LogComponent component = static_cast<LogComponent::Value>(i);
        testEncodedLogLine(MessageEventEphemeral(d, LogSeverity::Info(), component, ctx, msg),
                           str::stream() << " I " << component.getNameForLog() << " [");
    }
}

// Tests pass through of log component:
//     log macros -> LogStreamBuilder -> MessageEventEphemeral -> MessageEventDetailsEncoder
TEST_F(LogTestDetailsEncoder, ) {
    globalLogDomain()->setMinimumLoggedSeverity(LogSeverity::Log());

    // Default log component short name should not appear in detailed log line.
    MONGO_LOG_COMPONENT(0, componentDefault) << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(componentDefault.getNameForLog().toString()),
                      std::string::npos);

    // Non-default log component short name should appear in detailed log line.
    _logLines.clear();
    MONGO_LOG_COMPONENT(0, componentA) << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(componentA.getNameForLog().toString()), std::string::npos);

    // MONGO_LOG_COMPONENT2 - only the first component is sent to LogStreamBuilder.
    _logLines.clear();
    MONGO_LOG_COMPONENT2(0, componentA, componentB) << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(componentA.getNameForLog().toString()), std::string::npos);
    ASSERT_EQUALS(_logLines[0].find(componentB.getNameForLog().toString()), std::string::npos);

    // MONGO_LOG_COMPONENT3 - only the first component is sent to LogStreamBuilder.
    _logLines.clear();
    MONGO_LOG_COMPONENT3(0, componentA, componentB, componentC) << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(componentA.getNameForLog().toString()), std::string::npos);
    ASSERT_EQUALS(_logLines[0].find(componentB.getNameForLog().toString()), std::string::npos);
    ASSERT_EQUALS(_logLines[0].find(componentC.getNameForLog().toString()), std::string::npos);
}

// Tests pass through of log component:
//     unconditional log functions -> LogStreamBuilder -> MessageEventEphemeral
//                                 -> MessageEventDetailsEncoder
TEST_F(LogTestDetailsEncoder, LogFunctions) {
    // severe() - no component specified.
    severe() << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " F " << componentDefault.getNameForLog()),
                      std::string::npos);

    // severe() - with component.
    _logLines.clear();
    severe(componentA) << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " F " << componentA.getNameForLog()),
                      std::string::npos);

    // error() - no component specified.
    _logLines.clear();
    error() << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " E " << componentDefault.getNameForLog()),
                      std::string::npos);

    // error() - with component.
    _logLines.clear();
    error(componentA) << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " E " << componentA.getNameForLog()),
                      std::string::npos);

    // warning() - no component specified.
    _logLines.clear();
    warning() << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " W " << componentDefault.getNameForLog()),
                      std::string::npos);

    // warning() - with component.
    _logLines.clear();
    warning(componentA) << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " W " << componentA.getNameForLog()),
                      std::string::npos);

    // log() - no component specified.
    _logLines.clear();
    log() << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " I " << componentDefault.getNameForLog()),
                      std::string::npos);

    // log() - with component.
    _logLines.clear();
    log(componentA) << "This is logged";
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " I " << componentA.getNameForLog()),
                      std::string::npos);
}

}  // namespace
}  // namespace mongo
