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

#include <algorithm>
#include <cstdlib>

#include "mongo/base/status.h"
#include "mongo/logger/log_domain.h"

/*
 * Implementation of LogDomain<E>.  Include this in cpp files to instantiate new LogDomain types.
 * See message_log_domain.h, e.g.
 */

namespace mongo {
namespace logger {

template <typename E>
LogDomain<E>::LogDomain() : _abortOnFailure(false) {}

template <typename E>
LogDomain<E>::~LogDomain() {}

template <typename E>
Status LogDomain<E>::append(const E& event) {
    for (auto& appender : _appenders) {
        if (appender) {
            Status status = appender->append(event);
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
auto LogDomain<E>::attachAppender(std::unique_ptr<EventAppender> appender) -> AppenderHandle {
    const auto isValidPred = [](auto& appender) -> bool { return !appender; };
    auto iter = std::find_if(_appenders.begin(), _appenders.end(), isValidPred);

    if (iter == _appenders.end()) {
        _appenders.emplace_back(std::move(appender));
        return AppenderHandle(_appenders.size() - 1);
    }

    iter->swap(appender);
    return AppenderHandle(iter - _appenders.begin());
}

template <typename E>
auto LogDomain<E>::detachAppender(AppenderHandle handle) -> std::unique_ptr<EventAppender> {
    // So technically this could just return a moved unique_ptr reference
    // Still, eliding is a thing so swap is a nice certainty.
    std::unique_ptr<EventAppender> appender;
    using std::swap;
    swap(appender, _appenders.at(handle._index));
    return appender;
}

template <typename E>
void LogDomain<E>::clearAppenders() {
    _appenders.clear();
}

}  // namespace logger
}  // namespace mongo
