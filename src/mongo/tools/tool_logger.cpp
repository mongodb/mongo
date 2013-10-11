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

#include "mongo/tools/tool_logger.h"

#include <iostream>

#include "mongo/base/init.h"
#include "mongo/logger/console_appender.h"
#include "mongo/logger/log_manager.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/message_event.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/tools/tool_options.h"

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
