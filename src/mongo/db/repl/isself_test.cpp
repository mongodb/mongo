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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/isself.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
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
        ASSERT(isSelf(HostAndPort(*it, serverGlobalParams.port), getGlobalServiceContext()));
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
        ASSERT(isSelf(HostAndPort(*it, serverGlobalParams.port), getGlobalServiceContext()));
    }
#else
    ASSERT(true);
#endif
}

}  // namespace

}  // namespace repl
}  // namespace mongo
