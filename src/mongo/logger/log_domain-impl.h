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

#pragma once

#include <algorithm>
#include <cstdlib>

#include "mongo/base/status.h"
#include "mongo/logger/message_log_domain.h"

/*
 * Implementation of LogDomain<E>.  Include this in cpp files to instantiate new LogDomain types.
 * See message_log_domain.h, e.g.
 */

namespace mongo {
namespace logger {

template <typename E>
LogDomain<E>::LogDomain() : _abortOnFailure(false) {}

template <typename E>
LogDomain<E>::~LogDomain() {
    clearAppenders();
}

template <typename E>
Status LogDomain<E>::append(const E& event) {
    for (typename AppenderVector::const_iterator iter = _appenders.begin();
         iter != _appenders.end();
         ++iter) {
        if (*iter) {
            Status status = (*iter)->append(event);
            if (!status.isOK()) {
                if (_abortOnFailure) {
                    ::abort();
                }
                return status;
            }
        }
    }
    return Status::OK();
}

template <typename E>
typename LogDomain<E>::AppenderHandle LogDomain<E>::attachAppender(
    typename LogDomain<E>::AppenderAutoPtr appender) {
    typename AppenderVector::iterator iter =
        std::find(_appenders.begin(), _appenders.end(), static_cast<EventAppender*>(NULL));

    if (iter == _appenders.end()) {
        _appenders.push_back(appender.release());
        return AppenderHandle(_appenders.size() - 1);
    } else {
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
    for (typename AppenderVector::const_iterator iter = _appenders.begin();
         iter != _appenders.end();
         ++iter) {
        delete *iter;
    }

    _appenders.clear();
}

}  // namespace logger
}  // namespace mongo
