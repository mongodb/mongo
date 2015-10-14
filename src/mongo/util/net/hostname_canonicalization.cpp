/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/hostname_canonicalization.h"

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#endif

#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/text.h"

namespace mongo {

std::vector<std::string> getHostFQDNs(std::string hostName, HostnameCanonicalizationMode mode) {
#ifndef _WIN32
    using shim_char = char;
    using shim_addrinfo = struct addrinfo;
    const auto& shim_getaddrinfo = getaddrinfo;
    const auto& shim_freeaddrinfo = freeaddrinfo;
    const auto& shim_getnameinfo = getnameinfo;
    const auto& shim_toNativeString = [](const char* str) { return std::string(str); };
    const auto& shim_fromNativeString = [](const std::string& str) { return str; };
#else
    using shim_char = wchar_t;
    using shim_addrinfo = struct addrinfoW;
    const auto& shim_getaddrinfo = GetAddrInfoW;
    const auto& shim_freeaddrinfo = FreeAddrInfoW;
    const auto& shim_getnameinfo = GetNameInfoW;
    const auto& shim_toNativeString = toWideString;
    const auto& shim_fromNativeString = toUtf8String;
#endif

    std::vector<std::string> results;

    if (hostName.empty())
        return results;

    if (mode == HostnameCanonicalizationMode::kNone) {
        results.emplace_back(std::move(hostName));
        return results;
    }

    shim_addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = 0;
    hints.ai_protocol = 0;
    if (mode == HostnameCanonicalizationMode::kForward) {
        hints.ai_flags = AI_CANONNAME;
    }

    int err;
    shim_addrinfo* info;
    auto nativeHostName = shim_toNativeString(hostName.c_str());
    if ((err = shim_getaddrinfo(nativeHostName.c_str(), nullptr, &hints, &info)) != 0) {
        ONCE {
            warning() << "Failed to obtain address info for hostname " << hostName << ": "
                      << getAddrInfoStrError(err);
        }
        return results;
    }
    const auto guard = MakeGuard([&shim_freeaddrinfo, &info] { shim_freeaddrinfo(info); });

    if (mode == HostnameCanonicalizationMode::kForward) {
        results.emplace_back(shim_fromNativeString(info->ai_canonname));
        return results;
    }

    for (shim_addrinfo* p = info; p; p = p->ai_next) {
        shim_char host[NI_MAXHOST] = {};
        if ((err = shim_getnameinfo(
                 p->ai_addr, p->ai_addrlen, host, sizeof(host), nullptr, 0, NI_NAMEREQD)) == 0) {
            results.emplace_back(shim_fromNativeString(host));
        } else {
            // We can't do much better than this without writting some specific IPv4/6 address
            // handling logic.
            warning() << "Failed to obtain name info for an address: " << getAddrInfoStrError(err);
        }
    }

    // Deduplicate the results list
    std::sort(results.begin(), results.end());
    results.erase(std::unique(results.begin(), results.end()), results.end());

    // Remove any name that doesn't have a '.', since A records are illegal in TLDs
    results.erase(
        std::remove_if(results.begin(),
                       results.end(),
                       [](const std::string& str) { return str.find('.') == std::string::npos; }),
        results.end());

    return results;
}

}  // namespace mongo
