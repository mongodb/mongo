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


#include "mongo/util/net/hostname_canonicalization.h"

#if !defined(_WIN32)
#include <netdb.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include "mongo/logv2/log.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/text.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

StatusWith<std::vector<std::string>> getHostFQDNs(std::string hostName,
                                                  HostnameCanonicalizationMode mode) {
#ifndef _WIN32
    using shim_char = char;
    using shim_addrinfo = struct addrinfo;
    shim_addrinfo* info = nullptr;
    const auto& shim_getaddrinfo = getaddrinfo;
    const auto& shim_freeaddrinfo = [&info] {
        freeaddrinfo(info);
    };
    const auto& shim_getnameinfo = getnameinfo;
    const auto& shim_toNativeString = [](const char* str) {
        return std::string(str);
    };
    const auto& shim_fromNativeString = [](const std::string& str) {
        return str;
    };
#else
    using shim_char = wchar_t;
    using shim_addrinfo = struct addrinfoW;
    shim_addrinfo* info = nullptr;
    const auto& shim_getaddrinfo = GetAddrInfoW;
    const auto& shim_freeaddrinfo = [&info] {
        FreeAddrInfoW(info);
    };
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
    auto nativeHostName = shim_toNativeString(hostName.c_str());
    if ((err = shim_getaddrinfo(nativeHostName.c_str(), nullptr, &hints, &info)) != 0) {
        auto ec = addrInfoError(err);
        LOGV2_DEBUG(23170,
                    3,
                    "Failed to obtain address information for host",
                    "hostName"_attr = hostName,
                    "error"_attr = errorMessage(ec));

        return Status(ErrorCodes::BadValue, errorMessage(ec));
    }

    const ScopeGuard guard(shim_freeaddrinfo);

    if (mode == HostnameCanonicalizationMode::kForward) {
        results.emplace_back(shim_fromNativeString(info->ai_canonname));
        return results;
    }

    std::vector<std::string> getNameInfoErrors;
    for (shim_addrinfo* p = info; p; p = p->ai_next) {
        shim_char host[NI_MAXHOST] = {};
        if ((err = shim_getnameinfo(
                 p->ai_addr, p->ai_addrlen, host, sizeof(host), nullptr, 0, NI_NAMEREQD)) == 0) {
            results.emplace_back(shim_fromNativeString(host));
        } else {
            // Format the addrinfo structure we have into a string for reporting
            char ip_str[INET6_ADDRSTRLEN];
            struct sockaddr* addr = p->ai_addr;
            void* sin_addr = nullptr;

            if (p->ai_family == AF_INET) {
                struct sockaddr_in* addr_in = reinterpret_cast<struct sockaddr_in*>(addr);
                sin_addr = reinterpret_cast<void*>(&addr_in->sin_addr);
            } else if (p->ai_family == AF_INET6) {
                struct sockaddr_in6* addr_in6 = reinterpret_cast<struct sockaddr_in6*>(addr);
                sin_addr = reinterpret_cast<void*>(&addr_in6->sin6_addr);
            }

            std::stringstream getNameInfoError;
            if (sin_addr) {
                invariant(inet_ntop(p->ai_family, sin_addr, ip_str, sizeof(ip_str)) != nullptr);
                getNameInfoError << ip_str;
            } else {
                getNameInfoError << "Unknown address family: " << p->ai_family;
            }

            getNameInfoError << ": \"" << errorMessage(addrInfoError(err));
            getNameInfoErrors.push_back(getNameInfoError.str());
        }
    }

    if (!getNameInfoErrors.empty()) {
        LOGV2_DEBUG(23171, 3, "Failed to obtain name info", "errors"_attr = getNameInfoErrors);
    }

    LOGV2_DEBUG(7317600, 4, "Name info", "results"_attr = results);

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
