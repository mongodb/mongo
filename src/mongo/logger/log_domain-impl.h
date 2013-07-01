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

#include "mongo/logger/message_log_domain.h"

#include <algorithm>

/*
 * Implementation of LogDomain<E>.  Include this in cpp files to instantiate new LogDomain types.
 * See message_log_domain.h, e.g.
 */

namespace mongo {
namespace logger {

    template <typename E>
    LogDomain<E>::LogDomain() : _minimumLoggedSeverity(LogSeverity::Log()) {}

    template <typename E>
    LogDomain<E>::~LogDomain() {
        clearAppenders();
    }

    template <typename E>
    void LogDomain<E>::append(const E& event) {
        for (typename AppenderVector::const_iterator iter = _appenders.begin();
             iter != _appenders.end(); ++iter) {

            if (*iter) {
                (*iter)->append(event);
            }
        }
    }

    template <typename E>
    typename LogDomain<E>::AppenderHandle LogDomain<E>::attachAppender(
            typename LogDomain<E>::AppenderAutoPtr appender) {

        typename AppenderVector::iterator iter = std::find(
                _appenders.begin(),
                _appenders.end(),
                static_cast<EventAppender*>(NULL));

        if (iter == _appenders.end()) {
            _appenders.push_back(appender.release());
            return AppenderHandle(_appenders.size() - 1);
        }
        else {
            *iter = appender.release();
            return AppenderHandle(iter - _appenders.begin());
        }
    }

    template <typename E>
    typename LogDomain<E>::AppenderAutoPtr LogDomain<E>::detachAppender(
            typename LogDomain<E>::AppenderHandle handle) {

        EventAppender*& appender = _appenders.at(handle._index);
        AppenderAutoPtr result(appender);
        appender = NULL;
        return result;
    }

    template <typename E>
    void LogDomain<E>::clearAppenders() {
        for(typename AppenderVector::const_iterator iter = _appenders.begin();
            iter != _appenders.end(); ++iter) {

            delete *iter;
        }

        _appenders.clear();
    }

}  // namespace logger
}  // namespace mongo
