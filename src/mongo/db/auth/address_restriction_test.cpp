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

#include "mongo/db/auth/address_restriction.h"

#include "mongo/unittest/unittest.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/socket_utils.h"

#if !defined(_WIN32) && !defined(_WIN64)
#include <ifaddrs.h>
#include <netdb.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include <memory>

namespace mongo {
namespace {

std::string getLinkLocalIPv6Address(bool scopeOnly = false) {
#if !defined(_WIN32) && !defined(_WIN64)
    struct ifaddrs *ifaddr, *ifa;
    char addr_str[INET6_ADDRSTRLEN];
    std::string linkLocalWithScope;
    std::string scopeOnlyStr;

    if (getifaddrs(&ifaddr) == -1) {
        return std::string();
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET6) {
            continue;  // Skip non-IPv6 addresses
        }

        struct sockaddr_in6* sa = (struct sockaddr_in6*)ifa->ifa_addr;

        // Check if the address is link-local
        if (IN6_IS_ADDR_LINKLOCAL(&(sa->sin6_addr))) {
            inet_ntop(AF_INET6, &(sa->sin6_addr), addr_str, INET6_ADDRSTRLEN);
            linkLocalWithScope = std::string(addr_str) + "%" + ifa->ifa_name;
            linkLocalWithScope = scopeOnly ? ifa->ifa_name : linkLocalWithScope;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return linkLocalWithScope;
#else
    return std::string();
#endif
}

TEST(AddressRestrictionTest, toAndFromStringSingle) {
    const struct {
        std::string input;
        std::string output;
    } strings[] = {
        {"127.0.0.1/8", "127.0.0.1/8"},
        {"127.0.0.1", "127.0.0.1/32"},
        {"::1", "::1/128"},
        {"169.254.0.0/16", "169.254.0.0/16"},
        {"fe80::/10", "fe80::/10"},
        {.input = "fe80::1%eth0/10", .output = "fe80::1/10"},
        {.input = "fe80::8ff:ecff:fee6:fd4d%ens5/64", .output = "fe80::8ff:ecff:fee6:fd4d/64"}};
    for (const auto& p : strings) {
        {
            const ClientSourceRestriction csr(p.input);
            std::ostringstream actual;
            actual << csr;
            std::ostringstream expected;
            expected << "{\"clientSource\": [\"" << p.output << "\"]}";
            ASSERT_EQUALS(actual.str(), expected.str());
        }

        {
            const ServerAddressRestriction sar(p.input);
            std::ostringstream actual;
            actual << sar;
            std::ostringstream expected;
            expected << "{\"serverAddress\": [\"" << p.output << "\"]}";
            ASSERT_EQUALS(actual.str(), expected.str());
        }
    }
}

TEST(AddressRestrictionTest, toAndFromStringVector) {
    const struct {
        std::vector<std::string> input;
        std::string output;
    } tests[] = {
        {{"127.0.0.1", "169.254.0.0/16", "::1", "fe80::8ff:ecff:fee6:fd4d%ens5/64"},
         "[\"127.0.0.1/32\", \"169.254.0.0/16\", \"::1/128\", "
         "\"fe80::8ff:ecff:fee6:fd4d/64\"]"},
    };
    for (const auto& p : tests) {
        {
            const ClientSourceRestriction csr(p.input);
            std::ostringstream actual;
            actual << csr;
            std::ostringstream expected;
            expected << "{\"clientSource\": " << p.output << "}";
            ASSERT_EQUALS(actual.str(), expected.str());
        }

        {
            const ServerAddressRestriction sar(p.input);
            std::ostringstream actual;
            actual << sar;
            std::ostringstream expected;
            expected << "{\"serverAddress\": " << p.output << "}";
            ASSERT_EQUALS(actual.str(), expected.str());
        }
    }
}

TEST(AddressRestrictionTest, contains) {
    enableIPv6(true);
    const struct {
        std::vector<std::string> range;
        std::string address;
        bool valid;
    } contains[] = {
        {{"127.0.0.1/8"}, "0.0.0.0", false},
        {{"127.0.0.1/8"}, "0.0.0.1", false},
        {{"127.0.0.1/8"}, "126.255.255.255", false},
        {{"127.0.0.1/8"}, "127.0.0.0", true},
        {{"127.0.0.1/8"}, "127.0.0.1", true},
        {{"127.0.0.1/8"}, "127.0.0.127", true},
        {{"127.0.0.1/8"}, "127.0.0.128", true},
        {{"127.0.0.1/8"}, "127.0.0.255", true},
        {{"127.0.0.1/8"}, "127.255.255.255", true},
        {{"127.0.0.1/8"}, "127.127.128.1", true},
        {{"127.0.0.1/8"}, "127.128.127.0", true},
        {{"127.0.0.1/8"}, "128.0.0.1", false},
        {{"127.0.0.1/8"}, "169.254.13.37", false},
        {{"127.0.0.1/8"}, "255.0.0.1", false},
        {{"127.0.0.1/8"}, "255.255.255.254", false},
        {{"127.0.0.1/8"}, "255.255.255.255", false},

        {{"127.0.0.1"}, "126.255.255.255", false},
        {{"127.0.0.1"}, "127.0.0.0", false},
        {{"127.0.0.1"}, "126.0.0.1", false},
        {{"127.0.0.1"}, "127.0.0.1", true},
        {{"127.0.0.1"}, "127.0.0.2", false},
        {{"127.0.0.1"}, "127.0.0.127", false},
        {{"127.0.0.1"}, "127.0.0.128", false},
        {{"127.0.0.1"}, "127.0.0.255", false},
        {{"127.0.0.1"}, "127.1.0.1", false},
        {{"127.0.0.1"}, "127.128.0.1", false},
        {{"127.0.0.1"}, "128.0.0.1", false},

        {{"127.0.0.2/31"}, "127.0.0.0", false},
        {{"127.0.0.2/31"}, "127.0.0.1", false},
        {{"127.0.0.2/31"}, "127.0.0.2", true},
        {{"127.0.0.2/31"}, "127.0.0.3", true},
        {{"127.0.0.2/31"}, "127.0.0.4", false},
        {{"127.0.0.2/31"}, "127.0.1.1", false},

        {{"169.254.80.0/20"}, "169.254.79.255", false},
        {{"169.254.80.0/20"}, "169.254.80.0", true},
        {{"169.254.80.0/20"}, "169.254.95.255", true},
        {{"169.254.80.0/20"}, "169.254.96.0", false},

        {{"169.254.0.160/28"}, "169.254.0.159", false},
        {{"169.254.0.160/28"}, "169.254.0.160", true},
        {{"169.254.0.160/28"}, "169.254.0.175", true},
        {{"169.254.0.160/28"}, "169.254.0.176", false},

        {{"169.254.0.0/16"}, "8.8.8.8", false},
        {{"169.254.0.0/16"}, "127.0.0.1", false},
        {{"169.254.0.0/16"}, "169.254.0.0", true},
        {{"169.254.0.0/16"}, "169.254.0.1", true},
        {{"169.254.0.0/16"}, "169.254.31.17", true},
        {{"169.254.0.0/16"}, "169.254.255.254", true},
        {{"169.254.0.0/16"}, "169.254.255.255", true},
        {{"169.254.0.0/16"}, "240.0.0.1", false},
        {{"169.254.0.0/16"}, "255.255.255.255", false},

        {{"::1"}, "::", false},
        {{"::1"}, "::0", false},
        {{"::1"}, "::1", true},
        {{"::1"}, "::2", false},
        {{"::1"}, "::1:0", false},
        {{"::1"}, "::1:0:0", false},
        {{"::1"}, "1::", false},
        {{"::1"}, "1::1", false},
        {{"::1"}, "8000::1", false},

        {{"::1/96"}, "::1", true},
        {{"::1/96"}, "::1:0:0", false},
        {{"::1/96"}, "::DEAD:BEEF", true},
        {{"::1/96"}, "DEAD::BEEF", false},

        {{"::2/127"}, "::0", false},
        {{"::2/127"}, "::1", false},
        {{"::2/127"}, "::2", true},
        {{"::2/127"}, "::3", true},
        {{"::2/127"}, "::4", false},
        {{"::2/127"}, "::5", false},

        {{"::1/128"}, "::", false},
        {{"::1/128"}, "::0", false},
        {{"::1/128"}, "::1", true},
        {{"::1/128"}, "::2", false},
        {{"::1/128"}, "8000::", false},

        {{"127.0.0.1/8", "::1"}, "127.0.0.0", true},
        {{"127.0.0.1/8", "::1"}, "127.0.0.1", true},
        {{"127.0.0.1/8", "::1"}, "127.0.0.254", true},
        {{"127.0.0.1/8", "::1"}, "127.0.0.255", true},
        {{"127.0.0.1/8", "::1"}, "169.254.0.1", false},
        {{"127.0.0.1/8", "::1"}, "::1", true},
        {{"127.0.0.1/8", "::1"}, "::2", false},

        {{"169.254.0.0/16", "fe80::/10"}, "169.254.12.34", true},
        {{"169.254.0.0/16", "fe80::/10"}, "10.0.0.1", false},
        {{"169.254.0.0/16", "fe80::/10"}, "fe80::dead:beef", true},
        {{"169.254.0.0/16", "fe80::/10"}, "fec0::dead:beef", false},

        {{}, "127.0.0.1", false},
        {{}, "::1", false},
        {{}, "0.0.0.0", false},
        {{}, "::", false},
        {{}, "255.255.255.255", false},
        {{}, "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", false},

        {{"fe80::1/64"}, "fe80::8ff:ecff:fee6:fd4d", true},
    };
    for (const auto& p : contains) {
        const auto dummy = SockAddr();
        const auto addr = SockAddr::create(p.address, 1024, AF_UNSPEC);
        const RestrictionEnvironment rec(addr, dummy);
        const RestrictionEnvironment res(dummy, addr);

        const ClientSourceRestriction csr(p.range);
        ASSERT_EQ(csr.validate(rec).isOK(), p.valid);
        ASSERT_FALSE(csr.validate(res).isOK());

        const ServerAddressRestriction sar(p.range);
        ASSERT_EQ(sar.validate(res).isOK(), p.valid);
        ASSERT_FALSE(sar.validate(rec).isOK());
    }
}

TEST(AddressRestrictionTest, parseFail) {
    // Need IPv6 test?
    const BSONObj tests[] = {
        BSON("unknownField" << ""),
        BSON("clientSource" << "1.2.3.4.5"),
        BSON("clientSource" << "1.2.3.4"
                            << "unknownField"
                            << ""),
        BSON("clientSource" << "1.2.3.4"
                            << "clientSource"
                            << "2.3.4.5"),
    };
    for (const auto& t : tests) {
        ASSERT_FALSE(parseAddressRestrictionSet(t).isOK());
    }
}

TEST(AddressRestrictionTest, parseAndMatch) {
    const struct {
        // Restriction
        std::vector<std::string> clientSource;
        std::vector<std::string> serverAddress;
        // Environment
        std::string client;
        std::string server;
        // Should pass validation check
        bool valid;
    } tests[] = {
        {{"127.0.0.0/8"}, {"127.0.0.0/8"}, "127.0.0.1", "127.0.0.1", true},
        {{"127.0.0.0/8"}, {"127.0.0.0/8"}, "127.0.0.1", "169.254.1.2", false},
        {{"127.0.0.0/8"}, {"127.0.0.0/8"}, "169.254.3.4", "127.0.0.1", false},
        {{"127.0.0.0/8"}, {"127.0.0.0/8"}, "169.254.12.34", "169.254.56.78", false},

        {{"127.0.0.0/8"}, {}, "127.0.0.1", "169.254.5.6", true},
        {{"127.0.0.0/8"}, {}, "127.0.0.1", "127.0.0.1", true},
        {{"127.0.0.0/8"}, {}, "169.254.7.8", "127.0.0.1", false},
        {{"127.0.0.0/8"}, {}, "169.254.9.10", "169.254.11.12", false},

        {{}, {"127.0.0.0/8"}, "127.0.0.1", "169.254.5.6", false},
        {{}, {"127.0.0.0/8"}, "127.0.0.1", "127.0.0.1", true},
        {{}, {"127.0.0.0/8"}, "169.254.7.8", "127.0.0.1", true},
        {{}, {"127.0.0.0/8"}, "169.254.9.10", "169.254.11.12", false},

        {{"::/0"}, {"::/0"}, "127.0.0.1", "127.0.0.1", false},
        {{"0.0.0.0/0"}, {"0.0.0.0/0"}, "::1", "::1", false},

        {{"::1"}, {"::1"}, "::1", "::1", true},
        {{"::1"}, {"::1"}, "::2", "::1", false},
        {{"::1"}, {"::1"}, "::1", "::2", false},
        {{"::1"}, {"::1"}, "::2", "::2", false},

        {{"fe80::/10"}, {}, "fe80::dead:beef", "::1", true},
        {{"fe80::/10"}, {}, "fe80::dead:beef", "::ffff:127.0.0.1", true},
        {{"fe80::/10"}, {}, "fe80::dead:beef", "127.0.0.1", true},
        {{"fe80::/10"}, {}, "fe80::dead:beef", "fe80::ba5e:ba11", true},

        {{"fe80::/10"}, {}, "fec0::dead:beef", "fe80::1", false},
        {{"fe80::/10"}, {}, "fec0::dead:beef", "fe80::1", false},
        {{"fe80::/10"}, {}, "fec0::dead:beef", "fe80::1", false},
        {{"fe80::/10"}, {}, "fec0::dead:beef", "fe80::1", false},

        {{}, {"fe80::/10"}, "::1", "fe80::dead:beef", true},
        {{}, {"fe80::/10"}, "::ffff:127.0.0.1", "fe80::dead:beef", true},
        {{}, {"fe80::/10"}, "127.0.0.1", "fe80::dead:beef", true},
        {{}, {"fe80::/10"}, "fe80::ba5e:ba11", "fe80::dead:beef", true},

        {{"127.0.0.1/8", "::1"}, {"169.254.0.0/16", "fe80::/10"}, "::1", "fe80::1", true},
        {{"127.0.0.1/8", "::1"}, {"169.254.0.0/16", "fe80::/10"}, "127.0.0.1", "169.254.0.1", true},
        {{"127.0.0.1/8", "::1"}, {"169.254.0.0/16", "fe80::/10"}, "::1", "169.254.0.1", true},
        {{"127.0.0.1/8", "::1"}, {"169.254.0.0/16", "fe80::/10"}, "127.0.0.1", "fe80::1", true},

        {{"127.0.0.1/8", "::1"}, {"169.254.0.0/16", "fe80::/10"}, "::2", "fe80::1", false},
        {{"127.0.0.1/8", "::1"}, {"169.254.0.0/16", "fe80::/10"}, "10.0.0.1", "169.254.0.1", false},
        {{"127.0.0.1/8", "::1"}, {"169.254.0.0/16", "fe80::/10"}, "::1", "192.168.0.1", false},
        {{"127.0.0.1/8", "::1"}, {"169.254.0.0/16", "fe80::/10"}, "127.0.0.1", "fec0::1", false},
    };
    for (const auto& t : tests) {
        BSONObjBuilder b;
        if (!t.clientSource.empty()) {
            b.append("clientSource", t.clientSource);
        }
        if (!t.serverAddress.empty()) {
            b.append("serverAddress", t.serverAddress);
        }
        const auto doc = b.obj();
        const auto setwith = parseAddressRestrictionSet(doc);
        ASSERT_OK(setwith);

        const RestrictionEnvironment env(SockAddr::create(t.client, 1024, AF_UNSPEC),
                                         SockAddr::create(t.server, 1025, AF_UNSPEC));
        ASSERT_EQ(setwith.getValue().validate(env).isOK(), t.valid);
    }
}

TEST(AddressRestrictionTest, parseAndMatchLinkLocalSuccess) {
    // The IPv6 link-local address to use in the test is different for every OS, as well as every
    // instance of the OS. This will include the scope ID. We need to get this at runtime.
    std::string llAddr = getLinkLocalIPv6Address(false);
    if (llAddr.empty()) {
        // No link-local address found. Skip the test.
        return;
    }

    // Use the link-local address as the clientSource restriction
    std::vector<std::string> clientSourceVector = {llAddr};
    BSONObjBuilder b;
    b.append("clientSource", clientSourceVector);
    const auto doc = b.obj();
    const auto setwith = parseAddressRestrictionSet(doc);
    ASSERT_OK(setwith);

    const RestrictionEnvironment env(SockAddr::create(llAddr, 1024, AF_UNSPEC),
                                     SockAddr::create("fe80::1", 1025, AF_UNSPEC));
    ASSERT_TRUE(setwith.getValue().validate(env).isOK());
}

TEST(AddressRestrictionTest, parseAndMatchLinkLocalFail) {
    // The IPv6 link-local address to use in the test is different for every OS, as well as every
    // instance of the OS. This will include the scope ID. We need to get this at runtime.
    // For this test, we just need the scope ID.
    std::string llScope = getLinkLocalIPv6Address(true);
    if (llScope.empty()) {
        // No link-local address found. Skip the test.
        return;
    }
    // Client source will look like fe80::%ens5/10, where ens5 is the scope ID
    std::string clientSource = "fe80::";
    clientSource += "%" + llScope + "/10";
    std::vector<std::string> clientSourceVector = {clientSource};
    BSONObjBuilder b;
    b.append("clientSource", clientSourceVector);
    const auto doc = b.obj();
    const auto setwith = parseAddressRestrictionSet(doc);
    ASSERT_OK(setwith);

    const RestrictionEnvironment env(SockAddr::create("fec0::dead:beef", 1024, AF_UNSPEC),
                                     SockAddr::create("fe80::1", 1025, AF_UNSPEC));
    ASSERT_EQ(setwith.getValue().validate(env).isOK(), false);
}

TEST(AddressRestrictionTest, parseAndMatchLinkLocalFail2) {
    // The IPv6 link-local address to use in the test is different for every OS, as well as every
    // instance of the OS. This will include the scope ID. We need to get this at runtime.
    std::string llAddr = getLinkLocalIPv6Address(false);
    if (llAddr.empty()) {
        // No link-local address found. Skip the test.
        return;
    }

    std::string clientSource = llAddr;
    // Change the scope ID to an invalid interface name. This should cause the validation to fail.
    size_t percent = clientSource.find('%');
    if (percent != std::string::npos) {
        clientSource.replace(percent, clientSource.length() - percent, "%xxx");
    }

    // Use the link-local address as the clientSource restriction
    std::vector<std::string> clientSourceVector = {clientSource};
    BSONObjBuilder b;
    b.append("clientSource", clientSourceVector);
    const auto doc = b.obj();
    const auto setwith = parseAddressRestrictionSet(doc);
    ASSERT_OK(setwith);

    const RestrictionEnvironment env(SockAddr::create(llAddr, 1024, AF_UNSPEC),
                                     SockAddr::create("fe80::1", 1025, AF_UNSPEC));
    ASSERT_EQ(setwith.getValue().validate(env).isOK(), false);
}
}  // namespace
}  // namespace mongo
