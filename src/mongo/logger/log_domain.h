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

#pragma once

#include <boost/scoped_ptr.hpp>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/logger/appender.h"
#include "mongo/logger/log_severity.h"

namespace mongo {
namespace logger {

    /**
     * Logging domain for events of type E.
     *
     * A logging domain consists of a set of Appenders and a minimum severity.
     *
     * TODO: The severity doesn't seem to apply for auditing, maybe it only belongs on the
     * MessageLogManager?  We don't really have multiple tunable logging domains, right now.  Other
     * than the global domain, shouldLog doesn't matter.
     *
     * Usage: Configure the log domain in a single threaded context, using attachAppender,
     * detachAppender and clearAppenders().  The domain takes ownership of any attached appender,
     * returning an AppenderHandle for each attached appender.  That handle may be used later to
     * detach the appender, causing the domain to release ownership of it.  Mostly, this
     * attach/detach behavior is useful in testing, since you do not want to change the appenders of
     * a domain that is currently receiving append() calls.
     *
     * Once you've configured the domain, call append() from potentially many threads, to add log
     * messages.
     */
    template <typename E>
    class LogDomain {
        MONGO_DISALLOW_COPYING(LogDomain);
    public:
        typedef E Event;
        typedef Appender<Event> EventAppender;

        /**
         * Opaque handle returned by attachAppender(), which can be subsequently passed to
         * detachAppender() to detach an appender from an instance of LogDomain.
         */
        class AppenderHandle {
            friend class LogDomain;
        public:
            AppenderHandle() {}

        private:
            explicit AppenderHandle(size_t index) : _index(index) {}

            size_t _index;
        };

        // TODO(schwerin): Replace with unique_ptr in C++11.
        typedef std::auto_ptr<EventAppender> AppenderAutoPtr;

        LogDomain();
        ~LogDomain();

        /**
         * Receives an event for logging, calling append(event) on all attached appenders.
         *
         * TODO(schwerin): Should we return failed statuses somehow?  vector<AppenderHandle, Status>
         * for failed appends, e.g.?
         */
        void append(const Event& event);

        /**
         * Predicate that answers the question, "Should I, the caller, append to you, the log
         * domain, messages of the given severity?"  True means yes.
         */
        bool shouldLog(LogSeverity severity) { return severity >= _minimumLoggedSeverity; }

        /**
         * Gets the minimum severity of messages that should be sent to this LogDomain.
         */
        LogSeverity getMinimumLogSeverity() { return _minimumLoggedSeverity; }

        /**
         * Sets the minimum severity of messages that should be sent to this LogDomain.
         */
        void setMinimumLoggedSeverity(LogSeverity severity) { _minimumLoggedSeverity = severity; }


        //
        // Configuration methods.  Must be synchronized with each other and calls to "append" by the
        // caller.
        //

        /**
         * Attaches "appender" to this domain, taking ownership of it.  Returns a handle that may be
         * used later to detach this appender.
         */
        AppenderHandle attachAppender(AppenderAutoPtr appender);

        /**
         * Detaches the appender referenced by "handle" from this domain, releasing ownership of it.
         * Returns an auto_ptr to the handler to the caller, who is now responsible for its
         * deletion. Caller should consider "handle" is invalid after this call.
         */
        AppenderAutoPtr detachAppender(AppenderHandle handle);

        /**
         * Destroy all attached appenders, invalidating all handles.
         */
        void clearAppenders();

    private:
        typedef std::vector<EventAppender*> AppenderVector;

        LogSeverity _minimumLoggedSeverity;
        AppenderVector _appenders;
    };

}  // namespace logger
}  // namespace mongo
