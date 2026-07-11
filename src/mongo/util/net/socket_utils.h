// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/log_severity.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

inline constexpr Seconds kMaxKeepIdleSecs{300};
inline constexpr Seconds kMaxKeepIntvlSecs{1};


void setSocketKeepAliveParams(int sock,
                              logv2::LogSeverity errorLogSeverity,
                              Seconds maxKeepIdleSecs = kMaxKeepIdleSecs,
                              Seconds maxKeepIntvlSecs = kMaxKeepIntvlSecs);

std::string makeUnixSockPath(int port, std::string_view label = "");
std::string makeProxyUnixSockPath(int port, std::string_view prefix);

/**
 * Extracts the port number from the specified unix domain socket path name, under the assumption
 * that the path was produced by a call to makeUnixSockPath, which takes a port number as an
 * argument.
 * Returns -1 if an error occurs.
 * Note that this function assumes that the port passed to makeUnixSockPath was not negative.
 */
int parsePortFromUnixSockPath(std::string_view path);

inline bool isUnixDomainSocket(std::string_view hostname) {
    return hostname.find('/') != std::string::npos;
}

#ifndef _WIN32
void setUnixDomainSocketPermissions(const std::string& path, int permissions);
void setUnixDomainSocketGroup(const std::string& path, gid_t gid);
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
