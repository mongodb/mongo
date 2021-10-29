/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/transport/proxy_protocol_header_parser.h"

#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer.h"

namespace mongo::transport {
namespace {

using namespace unittest::match;
using namespace fmt::literals;

template <typename MSrc, typename MDst>
class ProxiedEndpointsAre : public Matcher {
public:
    explicit ProxiedEndpointsAre(MSrc&& s, MDst&& d) : _src(std::move(s)), _dst(std::move(d)) {}

    std::string describe() const {
        return "ProxiedEndpointsAre({}, {})"_format(_src.describe(), _dst.describe());
    }

    MatchResult match(const ProxiedEndpoints& e) const {
        return StructuredBindingsAre<MSrc, MDst>(_src, _dst).match(e);
    }

private:
    MSrc _src;
    MDst _dst;
};

ParserResults parseAllPrefixes(StringData s) {
    boost::optional<ParserResults> results;
    for (size_t len = 0; len <= s.size(); ++len) {
        StringData sub = s.substr(0, len);
        results = parseProxyProtocolHeader(sub);
        if (len < s.size()) {
            ASSERT_FALSE(results) << "size={}, sub={}"_format(len, sub);
        }
    }
    ASSERT_TRUE(results);
    return *results;
}

void parseStringExpectFailure(StringData s, std::string regex) {
    try {
        parseAllPrefixes(s);
        FAIL("Expected to throw");
    } catch (const DBException& ex) {
        ASSERT_THAT(ex.toStatus(), StatusIs(Eq(ErrorCodes::FailedToParse), ContainsRegex(regex)));
    }
}

boost::optional<ProxiedEndpoints> parseStringExpectSuccess(StringData s) {
    const ParserResults results = parseAllPrefixes(s);
    ASSERT_THAT(results.bytesParsed, Eq(s.size()));

    // Also test that adding garbage to the end doesn't increase the bytesParsed amount.
    const boost::optional<ParserResults> possibleResultsWithGarbage =
        parseProxyProtocolHeader(s + "garbage");
    ASSERT_TRUE(possibleResultsWithGarbage);
    const ParserResults resultsWithGarbage = *possibleResultsWithGarbage;
    ASSERT_THAT(resultsWithGarbage.bytesParsed, Eq(s.size()));
    if (results.endpoints) {
        ASSERT_THAT(*results.endpoints,
                    ProxiedEndpointsAre(Eq(resultsWithGarbage.endpoints->sourceAddress),
                                        Eq(resultsWithGarbage.endpoints->destinationAddress)));
    } else {
        ASSERT_FALSE(resultsWithGarbage.endpoints);
    }

    return resultsWithGarbage.endpoints;
}

TEST(ProxyProtocolHeaderParser, MalformedIpv4Addresses) {
    StringData testCases[] = {"1",
                              "1.1",
                              "1.1.1",
                              "1.1.1.1.1",
                              "1.1.1.1.",
                              ".1.1.1.1",
                              "1234.1.1.1",
                              "1.1234.1.1",
                              "1.1.1234.1",
                              "1.1.1.1234",
                              "1.1.1.a",
                              "1.1.1.256",
                              "256.1.1.1",
                              "1.1..1.1",
                              "-0.1.1.1",
                              "-1.1.1.1",
                              ""};

    for (const auto& testCase : testCases) {
        try {
            proxy_protocol_details::validateIpv4Address(testCase);
            FAIL("Expected to throw");
        } catch (const DBException& ex) {
            ASSERT_THAT(ex.toStatus(),
                        StatusIs(Eq(ErrorCodes::FailedToParse), ContainsRegex("malformed")));
        }
    }
}

TEST(ProxyProtocolHeaderParser, WellFormedIpv4Addresses) {
    StringData testCases[] = {
        "1.1.1.1", "0.0.0.0", "255.255.255.255", "0.255.0.255", "127.0.1.1", "1.12.123.0"};

    for (const auto& testCase : testCases) {
        proxy_protocol_details::validateIpv4Address(testCase);
    }
}

TEST(ProxyProtocolHeaderParser, MalformedIpv6Addresses) {
    StringData testCases[] = {"0000",
                              "0000:0000",
                              "0000:0000:0000",
                              "0000:0000:0000:0000:0000",
                              "0000:0000:0000:0000:0000:0000",
                              "0000:0000:0000:0000:0000:0000:0000",
                              "0000:0000:0000:0000:0000:0000:0000:0000:0000",
                              "0000:0000:0000:0000:0000:0000:0000:",
                              ":0000:0000:0000:0000:0000:0000:0000",
                              "00000:0000:0000:0000:0000:0000:0000:0000",
                              "0000:0000:0000:0000:0000:0000:0000:00000",
                              "0000:-0000:0000:0000:0000:0000:0000:0000",
                              "0000:-000:0000:0000:0000:0000:0000:0000",
                              "000g:0000:0000:0000:0000:0000:0000:0000",
                              "0000:0000:0000:0000:0000:0000:0000:000g",
                              "0000::0000:0000:0000:0000:0000:0000:0000",
                              "0000:0000:0000:0000:0000:0000:0000::0000",
                              "0000:0000:0000:0000:0000:0000:0000:0000::",
                              "::0000:0000:0000:0000:0000:0000:0000:0000",
                              "::0000::",
                              "0000::0000::0000:0000:0000:0000:0000",
                              "0000::0000::0000:0000:0000:0000:0000:0000",
                              "::0000:",
                              ":0000::",
                              ":::",
                              ""};

    for (const auto& testCase : testCases) {
        try {
            proxy_protocol_details::validateIpv6Address(testCase);
            FAIL("Expected to throw");
        } catch (const DBException& ex) {
            ASSERT_THAT(ex.toStatus(),
                        StatusIs(Eq(ErrorCodes::FailedToParse), ContainsRegex("malformed")));
        }
    }
}

TEST(ProxyProtocolHeaderParser, WellFormedIpv6Addresses) {
    StringData testCases[] = {"::",
                              "::0000",
                              "::0000:0000",
                              "::0000:0000:0000",
                              "::0000:0000:0000:0000",
                              "::0000:0000:0000:0000:0000",
                              "::0000:0000:0000:0000:0000:0000",
                              "::0000:0000:0000:0000:0000:0000:0000",
                              "0000:0000:0000:0000:0000:0000:0000::",
                              "0000:0000:0000:0000:0000:0000::",
                              "0000:0000:0000:0000:0000::",
                              "0000:0000:0000:0000::",
                              "0000:0000:0000::",
                              "0000:0000::",
                              "0000::",
                              "0000::0000",
                              "0000::0000:0000",
                              "0000::0000:0000:0000",
                              "0000::0000:0000:0000:0000",
                              "0000::0000:0000:0000:0000:0000",
                              "0000::0000:0000:0000:0000:0000:0000",
                              "0000:0000:0000::0000:0000:0000",
                              "0000:0000:0000::0000:0000",
                              "0000:0000:0000:0000:0000:0000:0000:0000",
                              "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff",
                              "ffff::",
                              "0123:4567:89ab:cdef::",
                              "::0123:4567:89ab:cdef",
                              "0123:4567::89ab:cdef"};

    for (const auto& testCase : testCases) {
        proxy_protocol_details::validateIpv6Address(testCase);
    }
}

TEST(ProxyProtocolHeaderParser, MalformedV1Headers) {
    std::pair<std::string, std::string> testCases[] = {
        {"PORXY ", "header bytes invalid"},

        {"PROXY " + std::string(200, '1'), "No terminating newline"},

        // Even if there is a terminating newline, it has to happen before the longest possible
        // header length is seen.
        {"PROXY UNKNOWN " + std::string(92, '1') + "\r\n", "No terminating newline"},

        {"PROXY " + std::string(50, '\r') + "1" + "\r\r\n", "address string malformed"},
        {"PROXY TCP4 \r\n", "address string malformed"},
        {"PROXY TCP4 1.1.1.1\r\n", "address string malformed"},
        {"PROXY TCP4 1.1.1.1 1.1.1.1 10\r\n", "address string malformed"},

        {"PROXY TCP4 12800000000 28 10 10\r\n", "malformed IPv4"},
        {"PROXY TCP4 128 28000000000000 10 10\r\n", "malformed IPv4"},
        {"PROXY TCP4 1.1.1.1 notanip 10 300\r\n", "malformed IPv4"},
        {"PROXY TCP4 a:b:c:d 20 10 300\r\n", "malformed IPv4"},
        {"PROXY TCP4 1.1.1.1 1.1.1.1 -10 300\r\n", "Negative"},
        {"PROXY TCP4 1.1.1.1 1.1.1.1 10 -300\r\n", "Negative"},
        {"PROXY TCP4 1.1.1.1 2.2.2.2 notaport 10\r\n", "Did not consume"},
        {"PROXY TCP4 1.1.1.1 2.2.2.2 10 20garbage\r\n", "Did not consume"},

        // Check TCP6
        {"PROXY TCP6 \r\n", "address string malformed"},
        {"PROXY TCP6 ::\r\n", "address string malformed"},
        {"PROXY TCP6 :: :: 10\r\n", "address string malformed"},

        {"PROXY TCP6 1.1.1.1 2.2.2.2 10 10\r\n", "malformed IPv6"},
        {"PROXY TCP6 :: ::000g 10 10\r\n", "malformed IPv6"},
        {"PROXY TCP6 :: :: -10 10\r\n", "Negative"},
        {"PROXY TCP6 :: :: 10 -10\r\n", "Negative"},
        {"PROXY TCP6 :: notanip 10 300\r\n", "malformed IPv6"},
        {"PROXY TCP6 :: :: notaport 10\r\n", "Did not consume"},
        {"PROXY TCP6 :: :: 10 20garbage\r\n", "Did not consume"}};

    for (const auto& testCase : testCases) {
        parseStringExpectFailure(testCase.first, testCase.second);
    }
}

TEST(ProxyProtocolHeaderParser, WellFormedV1Headers) {
    ASSERT_THAT(*parseStringExpectSuccess("PROXY TCP4 1.1.1.1 2.2.2.2 10 300\r\n"),
                ProxiedEndpointsAre(Eq(SockAddr::create("1.1.1.1", 10, AF_INET)),
                                    Eq(SockAddr::create("2.2.2.2", 300, AF_INET))));

    ASSERT_THAT(*parseStringExpectSuccess("PROXY TCP4 0.0.0.128 0.0.1.44 1000 3000\r\n"),
                ProxiedEndpointsAre(Eq(SockAddr::create("0.0.0.128", 1000, AF_INET)),
                                    Eq(SockAddr::create("0.0.1.44", 3000, AF_INET))));

    static constexpr StringData allFs = "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff"_sd;
    ASSERT_THAT(*parseStringExpectSuccess("PROXY TCP6 {} "
                                          "{} 10000 30000\r\n"_format(allFs, allFs)),
                ProxiedEndpointsAre(Eq(SockAddr::create(allFs, 10000, AF_INET6)),
                                    Eq(SockAddr::create(allFs, 30000, AF_INET6))));

    ASSERT_THAT(*parseStringExpectSuccess("PROXY TCP6 :: {} 1000 3000\r\n"_format(allFs)),
                ProxiedEndpointsAre(Eq(SockAddr::create("::", 1000, AF_INET6)),
                                    Eq(SockAddr::create(allFs, 3000, AF_INET6))));

    ASSERT_THAT(*parseStringExpectSuccess("PROXY TCP6 2001:0db8:: 0064:ff9b::0000 1000 3000\r\n"),
                ProxiedEndpointsAre(Eq(SockAddr::create("2001:db8::", 1000, AF_INET6)),
                                    Eq(SockAddr::create("64:ff9b::0000", 3000, AF_INET6))));

    // The shortest possible V1 header
    ASSERT_FALSE(parseStringExpectSuccess("PROXY UNKNOWN\r\n"));
    ASSERT_FALSE(
        parseStringExpectSuccess("PROXY UNKNOWN 2001:db8:: 64:ff9b::0.0.0.0 1000 3000\r\n"));
    ASSERT_FALSE(parseStringExpectSuccess("PROXY UNKNOWN hot garbage\r\n"));
    // The longest possible V1 header
    ASSERT_FALSE(
        parseStringExpectSuccess("PROXY UNKNOWN {} {} 65535 65535\r\n"_format(allFs, allFs)));
}

struct TestV2Header {
    std::string header;
    std::string versionAndCommand;
    std::string addressFamilyAndProtocol;
    std::string length;
    std::string firstAddr;
    std::string secondAddr;
    std::string metadata;

    std::string toString() const {
        return "{}{}{}{}{}{}{}"_format(header,
                                       versionAndCommand,
                                       addressFamilyAndProtocol,
                                       length,
                                       firstAddr,
                                       secondAddr,
                                       metadata);
    }
};

TEST(ProxyProtocolHeaderParser, MalformedV2Headers) {
    // These strings contain null characters in them, so we need string literals.
    using namespace std::string_literals;

    TestV2Header header;

    // Specify an invalid header.
    header.header = "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x48\x54\x0A"s;
    parseStringExpectFailure(header.toString(), "header bytes invalid");

    // Correct the header but break the version.
    header.header = "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A"s;
    header.versionAndCommand = "\x30";
    parseStringExpectFailure(header.toString(), "Invalid version");
    header.versionAndCommand = "\x22";
    parseStringExpectFailure(header.toString(), "Invalid version");
    // Correct the version/command but break the address family.
    header.versionAndCommand = "\x21";
    header.addressFamilyAndProtocol = "\x40";
    parseStringExpectFailure(header.toString(), "Invalid address");
    header.addressFamilyAndProtocol = "\x23";
    parseStringExpectFailure(header.toString(), "Invalid protocol");

    // TCP4
    // Set the length to 1.
    header.addressFamilyAndProtocol = "\x11";
    header.length = "\x00\x01"s;
    header.firstAddr = std::string(1, '\0');
    parseStringExpectFailure(header.toString(), "too short");
    // Set to the longest non-valid length (11).
    header.length = "\x00\x0B"s;
    header.firstAddr = std::string(6, '\0');
    header.secondAddr = std::string(5, '\0');
    parseStringExpectFailure(header.toString(), "too short");

    // TCP6
    // Set the length to 1.
    header.addressFamilyAndProtocol = "\x21";
    header.length = "\x00\x01"s;
    header.firstAddr = std::string(1, '\0');
    header.secondAddr = "";
    parseStringExpectFailure(header.toString(), "too short");
    // Set to the longest non-valid length (35).
    header.length = "\x00\x23"s;
    header.firstAddr = std::string(18, '\0');
    header.secondAddr = std::string(17, '\0');
    parseStringExpectFailure(header.toString(), "too short");

    // UNIX
    // Set the length to 1.
    header.addressFamilyAndProtocol = "\x31";
    header.length = "\x00\x01"s;
    header.firstAddr = std::string(1, '\0');
    header.secondAddr = "";
    parseStringExpectFailure(header.toString(), "too short");
    // Set to the longest non-valid length (35).
    header.length = "\x00\xD7"s;
    header.firstAddr = std::string(108, '\0');
    header.secondAddr = std::string(107, '\0');
    parseStringExpectFailure(header.toString(), "too short");
}

template <typename SockAddrUn = sockaddr_un>
std::pair<SockAddrUn, SockAddrUn> createTestSockAddrUn(std::string srcPath, std::string dstPath) {
    std::pair<SockAddrUn, SockAddrUn> addrs;
    addrs.first.sun_family = addrs.second.sun_family = AF_UNIX;

    memcpy(addrs.first.sun_path,
           srcPath.c_str(),
           std::min(srcPath.size(), sizeof(SockAddrUn::sun_path)));
    memcpy(addrs.second.sun_path,
           dstPath.c_str(),
           std::min(dstPath.size(), sizeof(SockAddrUn::sun_path)));

    return addrs;
}

std::string createTestUnixPathString(StringData path) {
    std::string out{path};
    out.resize(proxy_protocol_details::kMaxUnixPathLength);
    return out;
}

TEST(ProxyProtocolHeaderParser, WellFormedV2Headers) {
    // These strings contain null characters in them, so we need string literals.
    using namespace std::string_literals;

    TestV2Header header;
    // TCP4
    header.header = "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A"s;
    header.versionAndCommand = "\x21"s;
    header.addressFamilyAndProtocol = "\x12"s;
    header.length = "\x00\x0C"s;
    header.firstAddr = "\x0C\x22\x38\x4e\xac\x10"s;
    header.secondAddr = "\x00\x01\x04\xd2\x1f\x90"s;
    ASSERT_THAT(*parseStringExpectSuccess(header.toString()),
                ProxiedEndpointsAre(Eq(SockAddr::create("12.34.56.78", 1234, AF_INET)),
                                    Eq(SockAddr::create("172.16.0.1", 8080, AF_INET))));

    // We correctly ignore data at the end.
    header.length = "\x00\x0E"s;
    header.metadata = std::string(2, '\0');
    ASSERT_THAT(*parseStringExpectSuccess(header.toString()),
                ProxiedEndpointsAre(Eq(SockAddr::create("12.34.56.78", 1234, AF_INET)),
                                    Eq(SockAddr::create("172.16.0.1", 8080, AF_INET))));

    // If this is a local connection, return nothing.
    header.versionAndCommand = "\x20"s;
    ASSERT_FALSE(parseStringExpectSuccess(header.toString()));

    // TCP6
    header.versionAndCommand = "\x21"s;
    header.addressFamilyAndProtocol = "\x21"s;
    header.length = "\x00\x24"s;
    header.firstAddr = "\x20\x1\xd\xb8\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x64"s;
    header.secondAddr = "\xff\x9b\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x0\x4\xd2\x1f\x90"s;
    header.metadata = "";
    ASSERT_THAT(*parseStringExpectSuccess(header.toString()),
                ProxiedEndpointsAre(Eq(SockAddr::create("2001:db8::", 1234, AF_INET6)),
                                    Eq(SockAddr::create("64:ff9b::0.0.0.0", 8080, AF_INET6))));

    // We correctly ignore data at the end.
    header.length = "\x00\x55"s;
    header.metadata = std::string(49, '\1');
    ASSERT_THAT(*parseStringExpectSuccess(header.toString()),
                ProxiedEndpointsAre(Eq(SockAddr::create("2001:db8::", 1234, AF_INET6)),
                                    Eq(SockAddr::create("64:ff9b::0.0.0.0", 8080, AF_INET6))));

    // If this is a local connection, return nothing.
    header.versionAndCommand = "\x20"s;
    ASSERT_FALSE(parseStringExpectSuccess(header.toString()));

    // UNIX
    header.versionAndCommand = "\x21"s;
    header.addressFamilyAndProtocol = "\x31"s;
    header.length = "\x00\xD8"s;
    const std::string srcPath(sizeof(sockaddr_un::sun_path) / 2, '\1');
    const std::string dstPath(sizeof(sockaddr_un::sun_path) - 1, '\2');
    header.firstAddr = createTestUnixPathString(srcPath);
    header.secondAddr = createTestUnixPathString(dstPath);
    header.metadata = "";
    const auto addrs = createTestSockAddrUn(srcPath, dstPath);
    ASSERT_THAT(*parseStringExpectSuccess(header.toString()),
                ProxiedEndpointsAre(Eq(SockAddr((sockaddr*)&addrs.first, sizeof(sockaddr_un))),
                                    Eq(SockAddr((sockaddr*)&addrs.second, sizeof(sockaddr_un)))));

    // We correctly ignore data at the end.
    header.length = "\x01\x08";
    header.metadata = std::string(48, '\0');
    // Extraneous data at the end is correctly ingested and ignored.
    ASSERT_THAT(*parseStringExpectSuccess(header.toString()),
                ProxiedEndpointsAre(Eq(SockAddr((sockaddr*)&addrs.first, sizeof(sockaddr_un))),
                                    Eq(SockAddr((sockaddr*)&addrs.second, sizeof(sockaddr_un)))));

    // If this is a local connection, return nothing.
    header.versionAndCommand = "\x20"s;
    ASSERT_FALSE(parseStringExpectSuccess(header.toString()));

    // The family is not parsed if the connection is local.
    header.addressFamilyAndProtocol = "\xA2";
    ASSERT_FALSE(parseStringExpectSuccess(header.toString()));
}

struct TestSockAddrUnLinux {
    sa_family_t sun_family;
    char sun_path[108];

    friend bool operator==(const TestSockAddrUnLinux& a, const TestSockAddrUnLinux& b) {
        return a.sun_family == b.sun_family && !memcmp(a.sun_path, b.sun_path, sizeof(sun_path));
    }
};

TEST(ProxyProtocolHeaderParser, LinuxSockAddrUnParsing) {
    // Test the parser against a Linux-like sockaddr_un
    {
        const std::string srcPath(sizeof(TestSockAddrUnLinux::sun_path) / 2, '\1');
        const std::string dstPath(sizeof(TestSockAddrUnLinux::sun_path) - 1, '\2');
        const auto addrs = createTestSockAddrUn<TestSockAddrUnLinux>(srcPath, dstPath);
        ASSERT_THAT(proxy_protocol_details::parseSockAddrUn<TestSockAddrUnLinux>(
                        createTestUnixPathString(srcPath)),
                    Eq(addrs.first));
        ASSERT_THAT(proxy_protocol_details::parseSockAddrUn<TestSockAddrUnLinux>(
                        createTestUnixPathString(dstPath)),
                    Eq(addrs.second));
    }

    {
        const std::string srcPath(sizeof(TestSockAddrUnLinux::sun_path), '\0');
        const std::string dstPath(1, '\0');
        const auto addrs = createTestSockAddrUn<TestSockAddrUnLinux>(srcPath, dstPath);
        ASSERT_THAT(proxy_protocol_details::parseSockAddrUn<TestSockAddrUnLinux>(
                        createTestUnixPathString(srcPath)),
                    Eq(addrs.first));
        ASSERT_THAT(proxy_protocol_details::parseSockAddrUn<TestSockAddrUnLinux>(
                        createTestUnixPathString(dstPath)),
                    Eq(addrs.second));
    }
}

struct TestSockAddrUnMac {
    unsigned char sun_len;
    sa_family_t sun_family;
    char sun_path[104];

    friend bool operator==(const TestSockAddrUnMac& a, const TestSockAddrUnMac& b) {
        return a.sun_family == b.sun_family && a.sun_len == b.sun_len &&
            !memcmp(a.sun_path, b.sun_path, a.sun_len);
    }
};

TEST(ProxyProtocolHeaderParser, MacSockAddrUnParsing) {
    // Test the parser against a Mac-like sockaddr_un
    {
        const std::string srcPath(sizeof(TestSockAddrUnMac::sun_path) / 2, '\1');
        const std::string dstPath(sizeof(TestSockAddrUnMac::sun_path) - 1, '\2');
        const auto addrs = createTestSockAddrUn<TestSockAddrUnMac>(srcPath, dstPath);
        ASSERT_THAT(proxy_protocol_details::parseSockAddrUn<TestSockAddrUnMac>(
                        createTestUnixPathString(srcPath)),
                    Eq(addrs.first));
        ASSERT_THAT(proxy_protocol_details::parseSockAddrUn<TestSockAddrUnMac>(
                        createTestUnixPathString(dstPath)),
                    Eq(addrs.second));
    }

    {
        const std::string srcPath(1, '\1');
        const std::string dstPath = "";
        const auto addrs = createTestSockAddrUn<TestSockAddrUnMac>(srcPath, dstPath);
        ASSERT_THAT(proxy_protocol_details::parseSockAddrUn<TestSockAddrUnMac>(
                        createTestUnixPathString(srcPath)),
                    Eq(addrs.first));
        ASSERT_THAT(proxy_protocol_details::parseSockAddrUn<TestSockAddrUnMac>(
                        createTestUnixPathString(dstPath)),
                    Eq(addrs.second));
    }

    try {
        const std::string srcPath(proxy_protocol_details::kMaxUnixPathLength - 1, '\1');
        const std::string dstPath(proxy_protocol_details::kMaxUnixPathLength - 1, '\2');
        proxy_protocol_details::parseSockAddrUn<TestSockAddrUnMac>(
            createTestUnixPathString(srcPath) + createTestUnixPathString(dstPath));
        FAIL("Expected to throw");
    } catch (const DBException& ex) {
        ASSERT_THAT(ex.toStatus(),
                    StatusIs(Eq(ErrorCodes::FailedToParse), ContainsRegex("longer than system")));
    }

    try {
        const std::string srcPath(sizeof(TestSockAddrUnMac::sun_path), '\1');
        const std::string dstPath(sizeof(TestSockAddrUnMac::sun_path), '\2');
        proxy_protocol_details::parseSockAddrUn<TestSockAddrUnMac>(
            createTestUnixPathString(srcPath) + createTestUnixPathString(dstPath));
        FAIL("Expected to throw");
    } catch (const DBException& ex) {
        ASSERT_THAT(ex.toStatus(),
                    StatusIs(Eq(ErrorCodes::FailedToParse), ContainsRegex("longer than system")));
    }
}

}  // namespace
}  // namespace mongo::transport
