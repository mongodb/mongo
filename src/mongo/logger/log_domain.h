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

#include <list>
#include <memory>
#include <string>

#include "mongo/logger/appender.h"
#include "mongo/logv2/log_severity.h"

namespace mongo::logger {

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
    using Event = E;
    using AppenderList = std::list<std::unique_ptr<Appender<Event>>>;
    using AppendersIter = typename AppenderList::iterator;

public:
    /**
     * Opaque handle returned by attachAppender(), which can be subsequently passed to
     * detachAppender() to detach an appender from an instance of LogDomain.
     */
    class AppenderHandle {
    private:
        friend class LogDomain;
        explicit AppenderHandle(AppendersIter iter) : _iter{iter} {}
        AppendersIter _iter;
    };

    LogDomain() = default;
    ~LogDomain() = default;

    LogDomain(const LogDomain&) = delete;
    LogDomain& operator=(const LogDomain&) = delete;

    /**
     * Receives an event for logging, calling append(event) on all attached appenders.
     *
     * If any appender fails, the behavior is determined by the abortOnFailure flag:
     * *If abortOnFailure is set, ::abort() is immediately called.
     * *If abortOnFailure is not set, the error is returned and no further appenders are called.
     */
    Status append(const Event& event) {
        for (auto& appender : _appenders) {
            if (Status status = appender->append(event); !status.isOK()) {
                if (_abortOnFailure) {
                    ::abort();
                }
                return status;
            }
        }
        return Status::OK();
    }

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

    /**
     * Attaches `appender`. Returns a handle for use with `detachAppender`.
     */
    AppenderHandle attachAppender(std::unique_ptr<Appender<Event>> appender) {
        return AppenderHandle(_appenders.insert(_appenders.end(), std::move(appender)));
    }

    /**
     * Detaches the appender referenced by `handle`, returning it.
     */
    std::unique_ptr<Appender<Event>> detachAppender(AppenderHandle handle) {
        auto result = std::move(*handle._iter);
        _appenders.erase(handle._iter);
        return result;
    }

    /**
     * Destroy all attached appenders, invalidating all handles.
     */
    void clearAppenders() {
        _appenders.clear();
    }

private:
    AppenderList _appenders;
    bool _abortOnFailure = false;
};

}  // namespace mongo::logger
