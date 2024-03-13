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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/util/duration.h"

namespace mongo {

inline constexpr Seconds kMaxKeepIdleSecs{300};
inline constexpr Seconds kMaxKeepIntvlSecs{1};


void setSocketKeepAliveParams(int sock,
                              logv2::LogSeverity errorLogSeverity,
                              Seconds maxKeepIdleSecs = kMaxKeepIdleSecs,
                              Seconds maxKeepIntvlSecs = kMaxKeepIntvlSecs);

std::string makeUnixSockPath(int port, StringData label = "");

inline bool isUnixDomainSocket(StringData hostname) {
    return hostname.find('/') != std::string::npos;
}

#ifndef _WIN32
void setUnixDomainSocketPermissions(const std::string& path, int permissions);
#endif

// If an ip address is passed in, just return that.  If a hostname is passed
// in, look up its ip and return that.  Returns "" on failure.
std::string hostbyname(const char* hostname);

void enableIPv6(bool state = true);
bool IPv6Enabled();

/** this is not cache and does a syscall */
std::string getHostName();

/** this is cached, so if changes during the process lifetime
 * will be stale */
std::string getHostNameCached();


/**
 * Returns getHostNameCached():<port>.
 */
std::string prettyHostNameAndPort(int port);

/**
 * Returns getHostNameCached(), or getHostNameCached():<port> if running on a non-default port.
 */
std::string prettyHostName(int port);

}  // namespace mongo
