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

#include "mongo/db/repl/isself.h"

#include "mongo/base/string_data.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

namespace {

using std::string;

TEST_F(ServiceContextTest, DetectsSameHostIPv4) {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    bool wasEnabled = IPv6Enabled();
    enableIPv6(false);
    ON_BLOCK_EXIT([&] { enableIPv6(wasEnabled); });
    // first we get the addrs bound on this host
    const std::vector<std::string> addrs = getBoundAddrs(false);
    // Fastpath should agree with the result of getBoundAddrs
    // since it uses it...
    for (std::vector<string>::const_iterator it = addrs.begin(); it != addrs.end(); ++it) {
        ASSERT(isSelfFastPath(HostAndPort(*it, serverGlobalParams.port),
                              serverGlobalParams.priorityPort));
        ASSERT(isSelf(HostAndPort(*it, serverGlobalParams.port),
                      serverGlobalParams.priorityPort,
                      getGlobalServiceContext()));
    }
#else
    ASSERT(true);
#endif
}

TEST_F(ServiceContextTest, DetectsSameHostIPv6) {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    bool wasEnabled = IPv6Enabled();
    enableIPv6(true);
    ON_BLOCK_EXIT([&] { enableIPv6(wasEnabled); });
    // first we get the addrs bound on this host
    const std::vector<std::string> addrs = getBoundAddrs(true);
    // Fastpath should agree with the result of getBoundAddrs
    // since it uses it...
    for (std::vector<string>::const_iterator it = addrs.begin(); it != addrs.end(); ++it) {
        ASSERT(isSelfFastPath(HostAndPort(*it, serverGlobalParams.port),
                              serverGlobalParams.priorityPort));
        ASSERT(isSelf(HostAndPort(*it, serverGlobalParams.port),
                      serverGlobalParams.priorityPort,
                      getGlobalServiceContext()));
    }
#else
    ASSERT(true);
#endif
}

TEST_F(ServiceContextTest, RetryOnTransientDNSErrorsInFastPathEnoughAttempts) {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    bool wasEnabled = IPv6Enabled();
    enableIPv6(true);
    ON_BLOCK_EXIT([&] { enableIPv6(wasEnabled); });

    // First we get the addrs bound on this host
    const std::vector<std::string> addrs = getBoundAddrs(true);

    auto failResolution = globalFailPointRegistry().find("transientDNSErrorInFastPath");
    failResolution->setMode(FailPoint::nTimes, 3);

    // Fastpath should agree with the result of getBoundAddrs and should be able to resolve
    // the addresses even with the (transient) failures.
    for (std::vector<string>::const_iterator it = addrs.begin(); it != addrs.end(); ++it) {
        ASSERT(isSelfFastPath(HostAndPort(*it, serverGlobalParams.port),
                              serverGlobalParams.priorityPort));
    }
#else
    ASSERT(true);
#endif
}

TEST_F(ServiceContextTest, RetryOnTransientDNSErrorsInFastPathAttemptsExhausted) {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    bool wasEnabled = IPv6Enabled();
    enableIPv6(true);
    ON_BLOCK_EXIT([&] { enableIPv6(wasEnabled); });

    // First we get the addrs bound on this host
    const std::vector<std::string> addrs = getBoundAddrs(true);

    auto failResolution = globalFailPointRegistry().find("transientDNSErrorInFastPath");
    failResolution->setMode(FailPoint::nTimes, 5 * addrs.size());

    // Fastpath should not be able to resolve any of the addresses.
    for (std::vector<string>::const_iterator it = addrs.begin(); it != addrs.end(); ++it) {
        ASSERT_FALSE(isSelfFastPath(HostAndPort(*it, serverGlobalParams.port),
                                    serverGlobalParams.priorityPort));
    }
#else
    ASSERT(true);
#endif
}


TEST_F(ServiceContextTest, DoubleFreeOnGetaddrinfoFailureAfterRetry) {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    bool wasEnabled = IPv6Enabled();
    enableIPv6(true);
    ON_BLOCK_EXIT([&] { enableIPv6(wasEnabled); });

    const std::vector<std::string> addrs = getBoundAddrs(true);
    ASSERT_FALSE(addrs.empty());

    // skipGetaddrinfoCall in 'skip' mode with value 1: the first evaluation passes through
    // (allowing a real getaddrinfo that sets addrs to a valid pointer), then all subsequent
    // evaluations fire (skipping getaddrinfo, leaving addrs as a dangling freed pointer).
    auto skipGetaddrinfo = globalFailPointRegistry().find("skipGetaddrinfoCall");
    skipGetaddrinfo->setMode(FailPoint::skip, 1);
    ON_BLOCK_EXIT([&] { skipGetaddrinfo->setMode(FailPoint::off); });

    // transientDNSErrorInFastPath always fires, overriding err to EAI_AGAIN on every iteration.
    // Combined with skipGetaddrinfoCall, this creates the double-free scenario:
    //   Iter 1: getaddrinfo succeeds (addrs = valid ptr), failpoint -> EAI_AGAIN,
    //   freeaddrinfo(addrs) Iter 2: getaddrinfo skipped (addrs = dangling), failpoint -> EAI_AGAIN,
    //   freeaddrinfo(addrs) -> DOUBLE FREE
    auto failResolution = globalFailPointRegistry().find("transientDNSErrorInFastPath");
    failResolution->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([&] { failResolution->setMode(FailPoint::off); });

    // This call triggers the double-free in the buggy code. Under AddressSanitizer or
    // debug allocators, this will crash. With the RAII fix applied, it is safe.
    ASSERT_FALSE(isSelfFastPath(HostAndPort(addrs.front(), serverGlobalParams.port),
                                serverGlobalParams.priorityPort));
#else
    ASSERT(true);
#endif
}

}  // namespace

}  // namespace repl
}  // namespace mongo
