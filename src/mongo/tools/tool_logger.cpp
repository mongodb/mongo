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

#include "mongo/tools/tool_logger.h"

#include <iostream>

#include "mongo/base/init.h"
#include "mongo/logger/console_appender.h"
#include "mongo/logger/log_manager.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/message_event.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/tools/tool_options.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

    /*
     * Theory of operation:
     *
     * At process start, the loader initializes "consoleMutex" to NULL.  At some point during static
     * initialization, the static initialization process, running in the one and only extant thread,
     * allocates a new boost::mutex on the heap and assigns consoleMutex to point to it.  While
     * consoleMutex is still NULL, we know that there is only one thread extant, so it is safe to
     * skip locking the consoleMutex in the ErrorConsole constructor.  Once the mutex is initialized,
     * users of ErrorConsole can start acquiring it.
     */

    boost::mutex *consoleMutex = new boost::mutex;

}  // namespace

    ErrorConsole::ErrorConsole() : _consoleLock() {
        if (consoleMutex) {
            boost::unique_lock<boost::mutex> lk(*consoleMutex);
            lk.swap(_consoleLock);
        }
    }

    std::ostream& ErrorConsole::out() { return std::cerr; }

namespace {

    logger::MessageLogDomain* toolErrorOutput = NULL;
    logger::MessageLogDomain* toolNonErrorOutput = NULL;
    logger::MessageLogDomain* toolNonErrorDecoratedOutput = NULL;

}  // namespace

MONGO_INITIALIZER_GENERAL(ToolLogRedirection,
                            ("GlobalLogManager", "EndStartupOptionHandling"),
                            ("default"))(InitializerContext*) {

    using logger::MessageEventEphemeral;
    using logger::MessageEventDetailsEncoder;
    using logger::MessageLogDomain;
    using logger::ConsoleAppender;

    toolErrorOutput = logger::globalLogManager()->getNamedDomain("toolErrorOutput");
    toolNonErrorOutput = logger::globalLogManager()->getNamedDomain("toolNonErrorOutput");
    toolNonErrorDecoratedOutput =
        logger::globalLogManager()->getNamedDomain("toolNonErrorDecoratedOutput");

    // Errors in the tools always go to stderr
    toolErrorOutput->attachAppender(
            MessageLogDomain::AppenderAutoPtr(
                new ConsoleAppender<MessageEventEphemeral, ErrorConsole>(
                    new logger::MessageEventUnadornedEncoder)));

    // If we are outputting data to stdout, we may need to redirect all logging to stderr
    if (!toolGlobalParams.canUseStdout) {
        logger::globalLogDomain()->clearAppenders();
        logger::globalLogDomain()->attachAppender(MessageLogDomain::AppenderAutoPtr(
                    new ConsoleAppender<MessageEventEphemeral, ErrorConsole>(
                        new MessageEventDetailsEncoder)));
    }

    // Only put an appender on our informational messages if we did not use --quiet
    if (!toolGlobalParams.quiet) {
        if (toolGlobalParams.canUseStdout) {
            // If we can use stdout, we can use the ConsoleAppender with the default console
            toolNonErrorOutput->attachAppender(
                    MessageLogDomain::AppenderAutoPtr(
                        new ConsoleAppender<MessageEventEphemeral>(
                            new logger::MessageEventUnadornedEncoder)));

            toolNonErrorDecoratedOutput->attachAppender(
                    MessageLogDomain::AppenderAutoPtr(
                        new ConsoleAppender<MessageEventEphemeral>(
                            new logger::MessageEventDetailsEncoder)));
        }
        else {
            // If we cannot use stdout, we have to use ErrorConsole to redirect informational
            // messages to stderr
            toolNonErrorOutput->attachAppender(
                    MessageLogDomain::AppenderAutoPtr(
                            new ConsoleAppender<MessageEventEphemeral, ErrorConsole>(
                                    new logger::MessageEventUnadornedEncoder)));

            toolNonErrorDecoratedOutput->attachAppender(
                    MessageLogDomain::AppenderAutoPtr(
                            new ConsoleAppender<MessageEventEphemeral, ErrorConsole>(
                                    new logger::MessageEventDetailsEncoder)));
        }
    }

    return Status::OK();
}

    LogstreamBuilder toolInfoLog() {
        return LogstreamBuilder(toolNonErrorDecoratedOutput,
                                "",
                                logger::LogSeverity::Log());
    }

    LogstreamBuilder toolInfoOutput() {
        return LogstreamBuilder(toolNonErrorOutput,
                                "",
                                logger::LogSeverity::Log());
    }

    LogstreamBuilder toolError() {
        return LogstreamBuilder(toolErrorOutput,
                                "",
                                logger::LogSeverity::Log());
    }

}  // namespace mongo
