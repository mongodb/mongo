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

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"

namespace mongo {
namespace {

using ExemptionVector = std::vector<std::variant<CIDR, std::string>>;

template <typename T>
std::variant<CIDR, std::string> makeExemption(T exemption) {
    auto swCIDR = CIDR::parse(exemption);
    if (swCIDR.isOK()) {
        return swCIDR.getValue();
    } else {
        return std::string{exemption};
    }
}

std::shared_ptr<transport::Session> makeIPSession(StringData ip) {
    return transport::MockSession::create(HostAndPort(ip.toString(), 27017),
                                          SockAddr::create(ip, 27017, AF_INET),
                                          SockAddr(),
                                          nullptr);
}

#ifndef _WIN32
std::shared_ptr<transport::Session> makeUNIXSession(StringData path) {
    return transport::MockSession::create(HostAndPort(""_sd.toString(), -1),
                                          SockAddr::create(""_sd, -1, AF_UNIX),
                                          SockAddr::create(path, -1, AF_UNIX),
                                          nullptr);
}
#endif

TEST(MaxConnsOverride, NormalCIDR) {
    ExemptionVector cidrOnly{makeExemption("127.0.0.1"), makeExemption("10.0.0.0/24")};

    ASSERT_TRUE(makeIPSession("127.0.0.1")->shouldOverrideMaxConns(cidrOnly));
    ASSERT_TRUE(makeIPSession("10.0.0.35")->shouldOverrideMaxConns(cidrOnly));
    ASSERT_FALSE(makeIPSession("192.168.0.53")->shouldOverrideMaxConns(cidrOnly));
}

#ifndef _WIN32
TEST(MaxConnsOverride, UNIXPaths) {
    ExemptionVector mixed{makeExemption("127.0.0.1"),
                          makeExemption("10.0.0.0/24"),
                          makeExemption("/tmp/mongod.sock")};

    ASSERT_TRUE(makeIPSession("127.0.0.1")->shouldOverrideMaxConns(mixed));
    ASSERT_TRUE(makeIPSession("10.0.0.35")->shouldOverrideMaxConns(mixed));
    ASSERT_FALSE(makeIPSession("192.168.0.53")->shouldOverrideMaxConns(mixed));
    ASSERT_TRUE(makeUNIXSession("/tmp/mongod.sock")->shouldOverrideMaxConns(mixed));
    ASSERT_FALSE(makeUNIXSession("/tmp/other-mongod.sock")->shouldOverrideMaxConns(mixed));
}
#endif

}  // namespace
}  // namespace mongo
