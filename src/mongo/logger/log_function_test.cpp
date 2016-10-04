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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/logger/log_test.h"

#include <sstream>
#include <string>
#include <vector>

#include "mongo/logger/appender.h"
#include "mongo/logger/encoder.h"
#include "mongo/logger/log_component.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

using namespace mongo::logger;

namespace mongo {
namespace {

typedef LogTest<MessageEventDetailsEncoder> LogTestDetailsEncoder;

// Constants for log component test cases.
const LogComponent componentA = LogComponent::kCommand;
const LogComponent componentB = MONGO_LOG_DEFAULT_COMPONENT;

// Tests pass through of log component:
//     unconditional log functions -> LogStreamBuilder -> MessageEventEphemeral
//                                 -> MessageEventDetailsEncoder
// MONGO_DEFAULT_LOG_COMPONENT is set to kReplication before including util/log.h
// so non-debug logging without explicit component will log with kReplication instead
// of kDefault.
TEST_F(LogTestDetailsEncoder, LogFunctionsOverrideGlobalComponent) {
    // severe() - no component specified.
    severe() << "This is logged";
    ASSERT_TRUE(shouldLog(LogSeverity::Severe()));
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " F " << componentB.getNameForLog()),
                      std::string::npos);

    // severe() - with component.
    _logLines.clear();
    severe(componentA) << "This is logged";
    ASSERT_TRUE(logger::globalLogDomain()->shouldLog(componentA, LogSeverity::Severe()));
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " F " << componentA.getNameForLog()),
                      std::string::npos);

    // error() - no component specified.
    _logLines.clear();
    error() << "This is logged";
    ASSERT_TRUE(shouldLog(LogSeverity::Error()));
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " E " << componentB.getNameForLog()),
                      std::string::npos);

    // error() - with component.
    _logLines.clear();
    error(componentA) << "This is logged";
    ASSERT_TRUE(logger::globalLogDomain()->shouldLog(componentA, LogSeverity::Error()));
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " E " << componentA.getNameForLog()),
                      std::string::npos);

    // warning() - no component specified.
    _logLines.clear();
    warning() << "This is logged";
    ASSERT_TRUE(shouldLog(LogSeverity::Warning()));
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " W " << componentB.getNameForLog()),
                      std::string::npos);

    // warning() - with component.
    _logLines.clear();
    warning(componentA) << "This is logged";
    ASSERT_TRUE(logger::globalLogDomain()->shouldLog(componentA, LogSeverity::Warning()));
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " W " << componentA.getNameForLog()),
                      std::string::npos);

    // log() - no component specified.
    _logLines.clear();
    log() << "This is logged";
    ASSERT_TRUE(shouldLog(LogSeverity::Log()));
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " I " << componentB.getNameForLog()),
                      std::string::npos);

    // log() - with component.
    _logLines.clear();
    log(componentA) << "This is logged";
    ASSERT_TRUE(logger::globalLogDomain()->shouldLog(componentA, LogSeverity::Log()));
    ASSERT_EQUALS(1U, _logLines.size());
    ASSERT_NOT_EQUALS(_logLines[0].find(str::stream() << " I " << componentA.getNameForLog()),
                      std::string::npos);
}

}  // namespace
}  // namespace mongo
