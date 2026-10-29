// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/mock_session.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/cidr.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

template <typename T>
std::variant<CIDR, std::string> makeExemption(T exemption) {
    auto swCIDR = CIDR::parse(exemption);
    if (swCIDR.isOK()) {
        return swCIDR.getValue();
    } else {
        return std::string{exemption};
    }
}

std::shared_ptr<transport::Session> makeIPSession(std::string_view ip) {
    return transport::MockSession::create(HostAndPort(std::string{ip}, 27017),
                                          SockAddr::create(ip, 27017, AF_INET),
                                          SockAddr(),
                                          nullptr);
}

#ifndef _WIN32
std::shared_ptr<transport::Session> makeUNIXSession(std::string_view path) {
    return transport::MockSession::create(HostAndPort(std::string{""sv}, -1),
                                          SockAddr::create(""sv, -1, AF_UNIX),
                                          SockAddr::create(path, -1, AF_UNIX),
                                          nullptr);
}
#endif

TEST(MaxConnsOverride, NormalCIDR) {
    CIDRList cidrOnly{makeExemption("127.0.0.1"), makeExemption("10.0.0.0/24")};

    ASSERT_TRUE(makeIPSession("127.0.0.1")->isExemptedByCIDRList(cidrOnly));
    ASSERT_TRUE(makeIPSession("10.0.0.35")->isExemptedByCIDRList(cidrOnly));
    ASSERT_FALSE(makeIPSession("192.168.0.53")->isExemptedByCIDRList(cidrOnly));
}

#ifndef _WIN32
TEST(MaxConnsOverride, UNIXPaths) {
    CIDRList mixed{makeExemption("127.0.0.1"),
                   makeExemption("10.0.0.0/24"),
                   makeExemption("/tmp/mongod.sock")};

    ASSERT_TRUE(makeIPSession("127.0.0.1")->isExemptedByCIDRList(mixed));
    ASSERT_TRUE(makeIPSession("10.0.0.35")->isExemptedByCIDRList(mixed));
    ASSERT_FALSE(makeIPSession("192.168.0.53")->isExemptedByCIDRList(mixed));
    ASSERT_TRUE(makeUNIXSession("/tmp/mongod.sock")->isExemptedByCIDRList(mixed));
    ASSERT_FALSE(makeUNIXSession("/tmp/other-mongod.sock")->isExemptedByCIDRList(mixed));
}
#endif

}  // namespace
}  // namespace mongo
