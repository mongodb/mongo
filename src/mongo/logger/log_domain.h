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

#pragma once

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
 * A logging domain consists of a set of Appenders.
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
    typedef std::unique_ptr<EventAppender> AppenderAutoPtr;

    /**
     * Opaque handle returned by attachAppender(), which can be subsequently passed to
     * detachAppender() to detach an appender from an instance of LogDomain.
     */
    class AppenderHandle {
        friend class LogDomain;

        static const size_t invalid_handle = (size_t)-1;

    public:
        AppenderHandle() : _index(invalid_handle) {}

        explicit operator bool() const noexcept {
            return _index != invalid_handle;
        }

        void reset() {
            _index = invalid_handle;
        }

    private:
        explicit AppenderHandle(size_t index) : _index(index) {}

        size_t _index;
    };

    LogDomain();
    ~LogDomain();

    /**
     * Receives an event for logging, calling append(event) on all attached appenders.
     *
     * If any appender fails, the behavior is determined by the abortOnFailure flag:
     * *If abortOnFailure is set, ::abort() is immediately called.
     * *If abortOnFailure is not set, the error is returned and no further appenders are called.
     */
    Status append(const Event& event);

    /**
     * Gets the state of the abortOnFailure flag.
     */
    bool getAbortOnFailure() const {
        return _abortOnFailure;
    }

    /**
     * Sets the state of the abortOnFailure flag.
     */
    void setAbortOnFailure(bool abortOnFailure) {
        _abortOnFailure = abortOnFailure;
    }

    //
    // Configuration methods.  Must be synchronized with each other and calls to "append" by the
    // caller.
    //

    /**
     * Attaches "appender" to this domain, taking ownership of it.  Returns a handle that may be
     * used later to detach this appender.
     */
    AppenderHandle attachAppender(std::unique_ptr<EventAppender> appender);

    template <typename Ptr>
    AppenderHandle attachAppender(Ptr appender) {
        return attachAppender(std::unique_ptr<EventAppender>(std::move(appender)));
    }

    /**
     * Detaches the appender referenced by "handle" from this domain, releasing ownership of it.
     * Returns an unique_ptr to the handler to the caller, who is now responsible for its
     * deletion. Caller should consider "handle" is invalid after this call.
     */
    std::unique_ptr<EventAppender> detachAppender(AppenderHandle handle);

    /**
     * Destroy all attached appenders, invalidating all handles.
     */
    void clearAppenders();

private:
    std::vector<std::unique_ptr<EventAppender>> _appenders;
    bool _abortOnFailure;
};

}  // namespace logger
}  // namespace mongo
