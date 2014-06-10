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
#include "mongo/logger/log_tag.h"
#include "mongo/logger/log_tag_settings.h"
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

    // Constants for log tag test cases.
    const LogTag tagA = LogTag::kCommands;
    const LogTag tagB = LogTag::kAccessControl;
    const LogTag tagC = LogTag::kNetworking;

    // No log tag declared at file scope.
    // Tag severity configuration:
    //     LogTag::kDefault: 2
    TEST_F(LogTest, MongoLogMacroNoFileScopeLogTag) {
        globalLogDomain()->setMinimumLoggedSeverity(LogSeverity::Debug(2));

        LOG(2) << "This is logged";
        LOG(3) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG
        _logLines.clear();
        MONGO_LOG_TAG(2, tagA) << "This is logged";
        MONGO_LOG_TAG(3, tagA) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG2
        _logLines.clear();
        MONGO_LOG_TAG2(2, tagA, tagB) << "This is logged";
        MONGO_LOG_TAG2(3, tagA, tagB) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG3
        _logLines.clear();
        MONGO_LOG_TAG3(2, tagA, tagB, tagC) << "This is logged";
        MONGO_LOG_TAG3(3, tagA, tagB, tagC) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);
    }

    // Default log tag declared at inner namespace scope (tagB).
    // Tag severity configuration:
    //     LogTag::kDefault: 1
    //     tagB: 2
    namespace scoped_default_log_tag_test {

        // Set MONGO_LOG's default tag to tagB.
        MONGO_LOG_DEFAULT_TAG_FILE(tagB);

        TEST_F(LogTest, MongoLogMacroNamespaceScopeLogTagDeclared) {
            globalLogDomain()->setMinimumLoggedSeverity(LogSeverity::Debug(1));
            globalLogDomain()->setMinimumLoggedSeverity(tagB,
                                                        LogSeverity::Debug(2));

            // LOG - uses log tag (tagB) declared in MONGO_LOG_DEFAULT_TAG_FILE.
            LOG(2) << "This is logged";
            LOG(3) << "This is not logged";
            ASSERT_EQUALS(1U, _logLines.size());
            ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

            globalLogDomain()->clearMinimumLoggedSeverity(tagB);
        }

    } // namespace scoped_default_log_tag_test

    // Default log tag declared at function scope (tagA).
    // Tag severity configuration:
    //     LogTag::kDefault: 1
    //     tagA: 2
    TEST_F(LogTest, MongoLogMacroFunctionScopeLogTagDeclared) {
        globalLogDomain()->setMinimumLoggedSeverity(LogSeverity::Debug(1));
        globalLogDomain()->setMinimumLoggedSeverity(tagA, LogSeverity::Debug(2));

        // Set MONGO_LOG's default tag to tagA.
        MONGO_LOG_DEFAULT_TAG_LOCAL(tagA);

        // LOG - uses log tag (tagA) declared in MONGO_LOG_DEFAULT_TAG.
        LOG(2) << "This is logged";
        LOG(3) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG - log message tag matches function scope tag.
        _logLines.clear();
        MONGO_LOG_TAG(2, tagA) << "This is logged";
        MONGO_LOG_TAG(3, tagA) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG - log message tag not configured - fall back on LogTag::kDefault severity.
        _logLines.clear();
        MONGO_LOG_TAG(1, tagB) << "This is logged";
        MONGO_LOG_TAG(2, tagB) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG2
        _logLines.clear();
        MONGO_LOG_TAG2(2, tagA, tagB) << "This is logged";
        MONGO_LOG_TAG2(3, tagA, tagB) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG2 - reverse order.
        _logLines.clear();
        MONGO_LOG_TAG2(2, tagB, tagA) << "This is logged";
        MONGO_LOG_TAG2(3, tagB, tagA) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG2 - none of the log message tags configured - fall back on LogTag::kDefault.
        _logLines.clear();
        MONGO_LOG_TAG2(1, tagB, tagC) << "This is logged";
        MONGO_LOG_TAG2(2, tagB, tagC) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG3
        _logLines.clear();
        MONGO_LOG_TAG3(2, tagA, tagB, tagC) << "This is logged";
        MONGO_LOG_TAG3(3, tagA, tagB, tagC) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG3 - configured tag as 2nd tag.
        _logLines.clear();
        MONGO_LOG_TAG3(2, tagB, tagA, tagC) << "This is logged";
        MONGO_LOG_TAG3(3, tagB, tagA, tagC) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG3 - configured tag as 3rd tag.
        _logLines.clear();
        MONGO_LOG_TAG3(2, tagB, tagC, tagA) << "This is logged";
        MONGO_LOG_TAG3(3, tagB, tagC, tagA) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        // MONGO_LOG_TAG3 - none of the log message tags configured - fall back on LogTag::kDefault.
        _logLines.clear();
        MONGO_LOG_TAG3(1, tagB, tagC, LogTag::kIndexing) << "This is logged";
        MONGO_LOG_TAG3(2, tagB, tagC, LogTag::kIndexing) << "This is not logged";
        ASSERT_EQUALS(1U, _logLines.size());
        ASSERT_EQUALS(std::string("This is logged\n"), _logLines[0]);

        globalLogDomain()->clearMinimumLoggedSeverity(tagA);
    }

    //
    // Tag log level tests.
    // The global log manager holds the tag log level configuration for the global log domain.
    // LOG() and MONGO_LOG_TAG() macros in util/log.h determine at runtime if a log message
    // should be written to the log domain.
    //

    TEST_F(LogTest, LogTagSettingsMinimumLogSeverity) {
        LogTagSettings settings;
        ASSERT_TRUE(settings.hasMinimumLogSeverity(LogTag::kDefault));
        ASSERT_TRUE(settings.getMinimumLogSeverity(LogTag::kDefault) == LogSeverity::Log());
        for (int i = 0; i < int(LogTag::kNumLogTags); ++i) {
            LogTag tag = static_cast<LogTag::Value>(i);
            if (tag == LogTag::kDefault) { continue; }
            ASSERT_FALSE(settings.hasMinimumLogSeverity(tag));
        }

        // Override and clear minimum severity level.
        for (int i = 0; i < int(LogTag::kNumLogTags); ++i) {
            LogTag tag = static_cast<LogTag::Value>(i);
            LogSeverity severity = LogSeverity::Debug(2);

            // Override severity level.
            settings.setMinimumLoggedSeverity(tag, severity);
            ASSERT_TRUE(settings.hasMinimumLogSeverity(tag));
            ASSERT_TRUE(settings.getMinimumLogSeverity(tag) == severity);

            // Clear severity level.
            // Special case: when clearing LogTag::kDefault, the corresponding
            //               severity level is set to default values (ie. Log()).
            settings.clearMinimumLoggedSeverity(tag);
            if (tag == LogTag::kDefault) {
                ASSERT_TRUE(settings.hasMinimumLogSeverity(tag));
                ASSERT_TRUE(settings.getMinimumLogSeverity(LogTag::kDefault) == LogSeverity::Log());
            }
            else {
                ASSERT_FALSE(settings.hasMinimumLogSeverity(tag));
            }
        }
    }

    // Test for shouldLog() when the minimum logged severity is set only for LogTag::kDefault.
    TEST_F(LogTest, LogTagSettingsShouldLogDefaultLogTagOnly) {
        LogTagSettings settings;

        // Initial log severity for LogTag::kDefault is Log().
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Info()));
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Log()));
        ASSERT_FALSE(settings.shouldLog(LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(LogSeverity::Debug(2)));

        // If any tags are provided to shouldLog(), we should get the same outcome
        // because we have not configured any non-LogTag::kDefault tags.
        ASSERT_TRUE(settings.shouldLog(tagA, LogSeverity::Log()));
        ASSERT_FALSE(settings.shouldLog(tagA, LogSeverity::Debug(1)));

        // Set minimum logged severity so that Debug(1) messages are written to log domain.
        settings.setMinimumLoggedSeverity(LogTag::kDefault, LogSeverity::Debug(1));
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Info()));
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Log()));
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(LogSeverity::Debug(2)));

        // Same results when tags are supplied to shouldLog().
        ASSERT_TRUE(settings.shouldLog(tagA, LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(tagA, LogSeverity::Debug(2)));
    }

    // Test for shouldLog() when we have configured a single tag.
    // Also checks that severity level has been reverted to match LogTag::kDefault
    // after clearing level.
    // Minimum severity levels:
    // LogTag::kDefault: 1
    // tagA: 2
    TEST_F(LogTest, LogTagSettingsShouldLogSingleTag) {
        LogTagSettings settings;

        settings.setMinimumLoggedSeverity(LogTag::kDefault, LogSeverity::Debug(1));
        settings.setMinimumLoggedSeverity(tagA, LogSeverity::Debug(2));

        // Tags for log message: LogTag::kDefault only.
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(LogSeverity::Debug(2)));

        // Tags for log message: tagA only.
        ASSERT_TRUE(settings.shouldLog(tagA, LogSeverity::Debug(2)));
        ASSERT_FALSE(settings.shouldLog(tagA, LogSeverity::Debug(3)));

        // Clear severity level for tagA and check shouldLog() again.
        settings.clearMinimumLoggedSeverity(tagA);
        ASSERT_TRUE(settings.shouldLog(tagA, LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(tagA, LogSeverity::Debug(2)));
    }

    // Test for shouldLog() when we have configured multiple tags.
    // Minimum severity levels:
    // LogTag::kDefault: 1
    // tagA: 2
    // tagB: 0
    TEST_F(LogTest, LogTagSettingsShouldLogMultipleTagsConfigured) {
        LogTagSettings settings;

        settings.setMinimumLoggedSeverity(LogTag::kDefault, LogSeverity::Debug(1));
        settings.setMinimumLoggedSeverity(tagA, LogSeverity::Debug(2));
        settings.setMinimumLoggedSeverity(tagB, LogSeverity::Log());

        // Tags for log message: LogTag::kDefault only.
        ASSERT_TRUE(settings.shouldLog(LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(LogSeverity::Debug(2)));

        // Tags for log message: tagA only.
        ASSERT_TRUE(settings.shouldLog(tagA, LogSeverity::Debug(2)));
        ASSERT_FALSE(settings.shouldLog(tagA, LogSeverity::Debug(3)));

        // Tags for log message: tagB only.
        ASSERT_TRUE(settings.shouldLog(tagB, LogSeverity::Log()));
        ASSERT_FALSE(settings.shouldLog(tagB, LogSeverity::Debug(1)));

        // Tags for log message: tagC only.
        // Since a tag-specific minimum severity is not configured for tagC,
        // shouldLog() falls back on LogTag::kDefault.
        ASSERT_TRUE(settings.shouldLog(tagC, LogSeverity::Debug(1)));
        ASSERT_FALSE(settings.shouldLog(tagC, LogSeverity::Debug(2)));
    }

}  // namespace
}  // namespace mongo
