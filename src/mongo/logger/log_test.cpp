/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/basic.h"

#include <sstream>
#include <string>
#include <vector>

#include "mongo/logger/appender.h"
#include "mongo/logger/encoder.h"
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
        LogTest() {
            globalLogDomain()->clearAppenders();
            _appenderHandle = globalLogDomain()->attachAppender(
                    MessageLogDomain::AppenderAutoPtr(new LogTestAppender(this)));
        }

        virtual ~LogTest() { globalLogDomain()->detachAppender(_appenderHandle); }

    protected:
        std::vector<std::string> _logLines;

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

}  // namespace
}  // namespace mongo
