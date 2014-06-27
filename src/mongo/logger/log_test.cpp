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

#include "mongo/platform/basic.h"

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

using namespace mongo::logger;

namespace mongo {
namespace {

    // TODO(schwerin): Have logger write to a different log from the global log, so that tests can
    // redirect their global log output for examination.
    class LogTest : public unittest::Test {
        friend class LogTestAppender;
    public:
        LogTest() : _severityOld(globalLogDomain()->getMinimumLogSeverity()) {
            globalLogDomain()->clearAppenders();
            _appenderHandle = globalLogDomain()->attachAppender(
                    MessageLogDomain::AppenderAutoPtr(new LogTestAppender(this)));
        }

        virtual ~LogTest() {
            globalLogDomain()->detachAppender(_appenderHandle);
            globalLogDomain()->setMinimumLoggedSeverity(_severityOld);
        }

    protected:
        std::vector<std::string> _logLines;
        LogSeverity _severityOld;

    private:
        class LogTestAppender : public MessageLogDomain::EventAppender {
        public:
            explicit LogTestAppender(LogTest* ltest) : _ltest(ltest) {}
            virtual ~LogTestAppender() {}
            virtual Status append(const MessageLogDomain::Event& event) {
                std::ostringstream _os;
                if (!_encoder.encode(event, _os))
                    return Status(ErrorCodes::LogWriteFailed, "Failed to append to LogTestAppender.");
                _ltest->_logLines.push_back(_os.str());
                return Status::OK();
            }

        private:
            LogTest *_ltest;
            MessageEventUnadornedEncoder _encoder;
        };

        MessageLogDomain::AppenderHandle _appenderHandle;
    };

    TEST_F(LogTest, logContext) {
        logContext("WHA!");
        ASSERT_GREATER_THAN(_logLines.size(), 1U);
        ASSERT_NOT_EQUALS(_logLines[0].find("WHA!"), std::string::npos);

        // TODO(schwerin): Ensure that logContext rights a proper context to the log stream,
        // including the address of the logContext() function.
        //void const* logContextFn = reinterpret_cast<void const*>(logContext);
    }

    class CountAppender : public Appender<MessageEventEphemeral> {
    public:
        CountAppender() : _count(0) {}
        virtual ~CountAppender() {}

        virtual Status append(const MessageEventEphemeral& event) {
            ++_count;
            return Status::OK();
        }

        int getCount() { return _count; }

    private:
        int _count;
    };

    /** Simple tests for detaching appenders. */
    TEST_F(LogTest, DetachAppender) {
        MessageLogDomain::AppenderAutoPtr countAppender(new CountAppender);
        MessageLogDomain domain;

        // Appending to the domain before attaching the appender does not affect the appender.
        domain.append(MessageEventEphemeral(0ULL, LogSeverity::Log(), "", "1"));
        ASSERT_EQUALS(0, dynamic_cast<CountAppender*>(countAppender.get())->getCount());

        // Appending to the domain after attaching the appender does affect the appender.
        MessageLogDomain::AppenderHandle handle = domain.attachAppender(countAppender);
        domain.append(MessageEventEphemeral(0ULL, LogSeverity::Log(), "", "2"));
        countAppender = domain.detachAppender(handle);
        ASSERT_EQUALS(1, dynamic_cast<CountAppender*>(countAppender.get())->getCount());

        // Appending to the domain after detaching the appender does not affect the appender.
        domain.append(MessageEventEphemeral(0ULL, LogSeverity::Log(), "", "3"));
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
    TEST_F(LogTest, LogstreamBuilderReentrance) {
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
        B() { log() << "Exercising initializer time logging."; }
    } b;

    // Constants for log component test cases.
    const LogComponent componentA = LogComponent::kCommands;
    const LogComponent componentB = LogComponent::kAccessControl;
    const LogComponent componentC = LogComponent::kNetworking;

    // No log component declared at file scope.
    // Component severity configuration:
    //     LogComponent::kDefault: 2
    TEST_F(LogTest, MongoLogMacroNoFileScopeLogComponent) {
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

    // Default log component declared at inner namespace scope (componentB).
    // Component severity configuration:
    //     LogComponent::kDefault: 1
    //     componentB: 2
    namespace scoped_default_log_component_test {

        // Set MONGO_LOG's default component to componentB.
        MONGO_LOG_DEFAULT_COMPONENT_FILE(componentB);

        TEST_F(LogTest, MongoLogMacroNamespaceScopeLogComponentDeclared) {
            globalLogDomain()->setMinimumLoggedSeverity(LogSeverity::Debug(1));
            globalLogDomain()->setMinimumLoggedSeverity(componentB,
                                                        LogSeverity::Debug(2));

            // LOG - uses log component (componentB) declared in MONGO_LOG_DEFAULT_COMPONENT_FILE.
            LOG(2) << "This is logged";
            LOG(3) << "This is not logged";
            ASSERT_EQUALS(1U, _logLines.size());
            ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

            globalLogDomain()->clearMinimumLoggedSeverity(componentB);
        }

    } // namespace scoped_default_log_component_test

    // Default log component declared at function scope (componentA).
    // Component severity configuration:
    //     LogComponent::kDefault: 1
    //     componentA: 2
    TEST_F(LogTest, MongoLogMacroFunctionScopeLogComponentDeclared) {
        globalLogDomain()->setMinimumLoggedSeverity(LogSeverity::Debug(1));
        globalLogDomain()->setMinimumLoggedSeverity(componentA, LogSeverity::Debug(2));

        // Set MONGO_LOG's default component to componentA.
        MONGO_LOG_DEFAULT_COMPONENT_LOCAL(componentA);

        // LOG - uses log component (componentA) declared in MONGO_LOG_DEFAULT_COMPONENT.
        LOG(2) << "This is logged";
        LOG(3) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_COMPONENT - log message component matches function scope component.
        _logLines.clear();
        MONGO_LOG_COMPONENT(2, componentA) << "This is logged";
        MONGO_LOG_COMPONENT(3, componentA) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_COMPONENT - log message component not configured - fall back on
        // LogComponent::kDefault.
        _logLines.clear();
        MONGO_LOG_COMPONENT(1, componentB) << "This is logged";
        MONGO_LOG_COMPONENT(2, componentB) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_COMPONENT2
        _logLines.clear();
        MONGO_LOG_COMPONENT2(2, componentA, componentB) << "This is logged";
        MONGO_LOG_COMPONENT2(3, componentA, componentB) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_COMPONENT2 - reverse order.
        _logLines.clear();
        MONGO_LOG_COMPONENT2(2, componentB, componentA) << "This is logged";
        MONGO_LOG_COMPONENT2(3, componentB, componentA) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_COMPONENT2 - none of the log message components configured - fall back on
        // LogComponent::kDefault.
        _logLines.clear();
        MONGO_LOG_COMPONENT2(1, componentB, componentC) << "This is logged";
        MONGO_LOG_COMPONENT2(2, componentB, componentC) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_COMPONENT3
        _logLines.clear();
        MONGO_LOG_COMPONENT3(2, componentA, componentB, componentC) << "This is logged";
        MONGO_LOG_COMPONENT3(3, componentA, componentB, componentC) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_COMPONENT3 - configured component as 2nd component.
        _logLines.clear();
        MONGO_LOG_COMPONENT3(2, componentB, componentA, componentC) << "This is logged";
        MONGO_LOG_COMPONENT3(3, componentB, componentA, componentC) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_COMPONENT3 - configured component as 3rd component.
        _logLines.clear();
        MONGO_LOG_COMPONENT3(2, componentB, componentC, componentA) << "This is logged";
        MONGO_LOG_COMPONENT3(3, componentB, componentC, componentA) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_COMPONENT3 - none of the log message components configured - fall back on
        // LogComponent::kDefault.
        _logLines.clear();
        MONGO_LOG_COMPONENT3(1, componentB, componentC, LogComponent::kIndexing)
            << "This is logged";
        MONGO_LOG_COMPONENT3(2, componentB, componentC, LogComponent::kIndexing)
            << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        globalLogDomain()->clearMinimumLoggedSeverity(componentA);
    }

    //
    // Component log level tests.
    // The global log manager holds the component log level configuration for the global log domain.
    // LOG() and MONGO_LOG_COMPONENT() macros in util/log.h determine at runtime if a log message
    // should be written to the log domain.
    //

    TEST_F(LogTest, LogComponentSettingsMinimumLogSeverity) {
        LogComponentSettings settings;
        ASSERT_TRUE(settings.hasMinimumLogSeverity(LogComponent::kDefault));
        ASSERT_TRUE(settings.getMinimumLogSeverity(LogComponent::kDefault) == LogSeverity::Log());
        for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
            LogComponent component = static_cast<LogComponent::Value>(i);
            if (component == LogComponent::kDefault) { continue; }
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
            }
            else {
                ASSERT_FALSE(settings.hasMinimumLogSeverity(component));
            }
        }
    }

    // Test for shouldLog() when the minimum logged severity is set only for LogComponent::kDefault.
    TEST_F(LogTest, LogComponentSettingsShouldLogDefaultLogComponentOnly) {
        LogComponentSettings settings;

        // Initial log severity for LogComponent::kDefault is Log().
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Info()));
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Log()));
        ASSERT_FALSE(settings.shouldLog(LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(LogSeverity::Debug(2)));

        // If any components are provided to shouldLog(), we should get the same outcome
        // because we have not configured any non-LogComponent::kDefault components.
        ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Log()));
        ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(1)));

        // Set minimum logged severity so that Debug(1) messages are written to log domain.
        settings.setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Debug(1));
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Info()));
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Log()));
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(LogSeverity::Debug(2)));

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
    TEST_F(LogTest, LogComponentSettingsShouldLogSingleComponent) {
        LogComponentSettings settings;

        settings.setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Debug(1));
        settings.setMinimumLoggedSeverity(componentA, LogSeverity::Debug(2));

        // Components for log message: LogComponent::kDefault only.
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(LogSeverity::Debug(2)));

        // Components for log message: componentA only.
        ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Debug(2)));
        ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(3)));

        // Clear severity level for componentA and check shouldLog() again.
        settings.clearMinimumLoggedSeverity(componentA);
        ASSERT_TRUE(settings.shouldLog(componentA, LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(componentA, LogSeverity::Debug(2)));
    }

    // Test for shouldLog() when we have configured multiple components.
    // Minimum severity levels:
    // LogComponent::kDefault: 1
    // componentA: 2
    // componentB: 0
    TEST_F(LogTest, LogComponentSettingsShouldLogMultipleComponentsConfigured) {
        LogComponentSettings settings;

        settings.setMinimumLoggedSeverity(LogComponent::kDefault, LogSeverity::Debug(1));
        settings.setMinimumLoggedSeverity(componentA, LogSeverity::Debug(2));
        settings.setMinimumLoggedSeverity(componentB, LogSeverity::Log());

        // Components for log message: LogComponent::kDefault only.
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(LogSeverity::Debug(2)));

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
    }

}  // namespace
}  // namespace mongo
