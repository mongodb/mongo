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


#include <fstream>

#include "mongo/config.h"
#include "mongo/platform/basic.h"

#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/net/ssl/context.hpp"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"

#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/ssl_types.h"

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
#include "mongo/util/net/dh_openssl.h"
#include "mongo/util/net/ssl/context_openssl.hpp"
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

// Test implementation needed by ASIO transport.
class ServiceEntryPointUtil : public ServiceEntryPoint {
public:
    void startSession(std::shared_ptr<transport::Session> session) override {
        stdx::unique_lock<Latch> lk(_mutex);
        _sessions.push_back(std::move(session));
        LOGV2(2303202, "started session");
        _cv.notify_one();
    }

    void endAllSessions(transport::Session::TagMask tags) override {
        LOGV2(2303302, "end all sessions");
        std::vector<std::shared_ptr<transport::Session>> old_sessions;
        {
            stdx::unique_lock<Latch> lock(_mutex);
            old_sessions.swap(_sessions);
        }
        old_sessions.clear();
    }

    Status start() override {
        return Status::OK();
    }

    bool shutdown(Milliseconds timeout) override {
        return true;
    }

    void appendStats(BSONObjBuilder*) const override {}

    size_t numOpenSessions() const override {
        stdx::unique_lock<Latch> lock(_mutex);
        return _sessions.size();
    }

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request) noexcept override {
        MONGO_UNREACHABLE;
    }

    logv2::LogSeverity slowSessionWorkflowLogSeverity() override {
        MONGO_UNIMPLEMENTED;
    }

    void setTransportLayer(transport::TransportLayer* tl) {
        _transport = tl;
    }

    void waitForConnect() {
        stdx::unique_lock<Latch> lock(_mutex);
        _cv.wait(lock, [&] { return !_sessions.empty(); });
    }

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("::_mutex");
    stdx::condition_variable _cv;
    std::vector<std::shared_ptr<transport::Session>> _sessions;
    transport::TransportLayer* _transport = nullptr;
};

std::string loadFile(const std::string& name) {
    std::ifstream input(name);
    std::string str((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return str;
}

TEST(SSLManager, matchHostname) {
    enum Expected : bool { match = true, mismatch = false };
    const struct {
        Expected expected;
        std::string hostname;
        std::string certName;
    } tests[] = {
        // clang-format off
        // Matches?  |    Hostname and possibly FQDN   |  Certificate name
        {match,                    "foo.bar.bas" ,           "*.bar.bas."},
        {mismatch,       "foo.subdomain.bar.bas" ,           "*.bar.bas."},
        {match,                    "foo.bar.bas.",           "*.bar.bas."},
        {mismatch,       "foo.subdomain.bar.bas.",           "*.bar.bas."},

        {match,                    "foo.bar.bas" ,           "*.bar.bas"},
        {mismatch,       "foo.subdomain.bar.bas" ,           "*.bar.bas"},
        {match,                    "foo.bar.bas.",           "*.bar.bas"},
        {mismatch,       "foo.subdomain.bar.bas.",           "*.bar.bas"},

        {mismatch,                "foo.evil.bas" ,           "*.bar.bas."},
        {mismatch,      "foo.subdomain.evil.bas" ,           "*.bar.bas."},
        {mismatch,                "foo.evil.bas.",           "*.bar.bas."},
        {mismatch,      "foo.subdomain.evil.bas.",           "*.bar.bas."},

        {mismatch,                "foo.evil.bas" ,           "*.bar.bas"},
        {mismatch,      "foo.subdomain.evil.bas" ,           "*.bar.bas"},
        {mismatch,                "foo.evil.bas.",           "*.bar.bas"},
        {mismatch,      "foo.subdomain.evil.bas.",           "*.bar.bas"},
        // clang-format on
    };
    bool failure = false;
    for (const auto& test : tests) {
        if (bool(test.expected) != hostNameMatchForX509Certificates(test.hostname, test.certName)) {
            failure = true;
            LOGV2_DEBUG(23266,
                        1,
                        "Failure for Hostname: {test_hostname} Certificate: {test_certName}",
                        "test_hostname"_attr = test.hostname,
                        "test_certName"_attr = test.certName);
        } else {
            LOGV2_DEBUG(23267,
                        1,
                        "Passed for Hostname: {test_hostname} Certificate: {test_certName}",
                        "test_hostname"_attr = test.hostname,
                        "test_certName"_attr = test.certName);
        }
    }
    ASSERT_FALSE(failure);
}

std::vector<RoleName> getSortedRoles(const stdx::unordered_set<RoleName>& roles) {
    std::vector<RoleName> vec;
    vec.reserve(roles.size());
    std::copy(roles.begin(), roles.end(), std::back_inserter<std::vector<RoleName>>(vec));
    std::sort(vec.begin(), vec.end());
    return vec;
}

TEST(SSLManager, MongoDBRolesParser) {
    /*
    openssl asn1parse -genconf mongodbroles.cnf -out foo.der

    -------- mongodbroles.cnf --------
    asn1 = SET:MongoDBAuthorizationGrant

    [MongoDBAuthorizationGrant]
    grant1 = SEQUENCE:MongoDBRole

    [MongoDBRole]
    role  = UTF8:role_name
    database = UTF8:Third field
    */
    // Positive: Simple parsing test
    {
        unsigned char derData[] = {0x31, 0x1a, 0x30, 0x18, 0x0c, 0x09, 0x72, 0x6f, 0x6c, 0x65,
                                   0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x0c, 0x0b, 0x54, 0x68, 0x69,
                                   0x72, 0x64, 0x20, 0x66, 0x69, 0x65, 0x6c, 0x64};
        auto swPeer = parsePeerRoles(ConstDataRange(derData));
        ASSERT_OK(swPeer.getStatus());
        auto item = *(swPeer.getValue().begin());
        ASSERT_EQ(item.getRole(), "role_name");
        ASSERT_EQ(item.getDB(), "Third field");
    }

    // Positive: Very long role_name, and long form lengths
    {
        unsigned char derData[] = {
            0x31, 0x82, 0x01, 0x3e, 0x30, 0x82, 0x01, 0x3a, 0x0c, 0x82, 0x01, 0x29, 0x72, 0x6f,
            0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61,
            0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c,
            0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d,
            0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65,
            0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65,
            0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f,
            0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72,
            0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e,
            0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f,
            0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61,
            0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c,
            0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d,
            0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65,
            0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65,
            0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f,
            0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72,
            0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e,
            0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f,
            0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61,
            0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c,
            0x65, 0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d,
            0x65, 0x0c, 0x0b, 0x54, 0x68, 0x69, 0x72, 0x64, 0x20, 0x66, 0x69, 0x65, 0x6c, 0x64};
        auto swPeer = parsePeerRoles(ConstDataRange(derData));
        ASSERT_OK(swPeer.getStatus());

        auto item = *(swPeer.getValue().begin());
        ASSERT_EQ(item.getRole(),
                  "role_namerole_namerole_namerole_namerole_namerole_namerole_namerole_namerole_"
                  "namerole_namerole_namerole_namerole_namerole_namerole_namerole_namerole_"
                  "namerole_namerole_namerole_namerole_namerole_namerole_namerole_namerole_"
                  "namerole_namerole_namerole_namerole_namerole_namerole_namerole_namerole_name");
        ASSERT_EQ(item.getDB(), "Third field");
    }

    // Negative: Encode MAX_INT64 into a length
    {
        unsigned char derData[] = {0x31, 0x88, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0x3e, 0x18, 0x0c, 0x09, 0x72, 0x6f, 0x6c, 0x65, 0x5f,
                                   0x6e, 0x61, 0x6d, 0x65, 0x0c, 0x0b, 0x54, 0x68, 0x69, 0x72,
                                   0x64, 0x20, 0x66, 0x69, 0x65, 0x6c, 0x64};

        auto swPeer = parsePeerRoles(ConstDataRange(derData));
        ASSERT_NOT_OK(swPeer.getStatus());
    }

    // Negative: Runt, only a tag
    {
        unsigned char derData[] = {0x31};
        auto swPeer = parsePeerRoles(ConstDataRange(derData));
        ASSERT_NOT_OK(swPeer.getStatus());
    }

    // Negative: Runt, only a tag and short length
    {
        unsigned char derData[] = {0x31, 0x0b};
        auto swPeer = parsePeerRoles(ConstDataRange(derData));
        ASSERT_NOT_OK(swPeer.getStatus());
    }

    // Negative: Runt, only a tag and long length with wrong missing length
    {
        unsigned char derData[] = {
            0x31,
            0x88,
            0xff,
            0xff,
        };
        auto swPeer = parsePeerRoles(ConstDataRange(derData));
        ASSERT_NOT_OK(swPeer.getStatus());
    }

    // Negative: Runt, only a tag and long length
    {
        unsigned char derData[] = {
            0x31,
            0x82,
            0xff,
            0xff,
        };
        auto swPeer = parsePeerRoles(ConstDataRange(derData));
        ASSERT_NOT_OK(swPeer.getStatus());
    }

    // Negative: Single UTF8 String
    {
        unsigned char derData[] = {
            0x0c, 0x0b, 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64};
        auto swPeer = parsePeerRoles(ConstDataRange(derData));
        ASSERT_NOT_OK(swPeer.getStatus());
    }

    // Negative: Unknown type - IAString
    {
        unsigned char derData[] = {
            0x16, 0x0b, 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64};
        auto swPeer = parsePeerRoles(ConstDataRange(derData));
        ASSERT_NOT_OK(swPeer.getStatus());
    }

    // Positive: two roles
    {
        unsigned char derData[] = {0x31, 0x2b, 0x30, 0x0f, 0x0c, 0x06, 0x62, 0x61, 0x63,
                                   0x6b, 0x75, 0x70, 0x0c, 0x05, 0x61, 0x64, 0x6d, 0x69,
                                   0x6e, 0x30, 0x18, 0x0c, 0x0f, 0x72, 0x65, 0x61, 0x64,
                                   0x41, 0x6e, 0x79, 0x44, 0x61, 0x74, 0x61, 0x62, 0x61,
                                   0x73, 0x65, 0x0c, 0x05, 0x61, 0x64, 0x6d, 0x69, 0x6e};
        auto swPeer = parsePeerRoles(ConstDataRange(derData));
        ASSERT_OK(swPeer.getStatus());

        auto roles = getSortedRoles(swPeer.getValue());
        ASSERT_EQ(roles[0].getRole(), "backup");
        ASSERT_EQ(roles[0].getDB(), "admin");
        ASSERT_EQ(roles[1].getRole(), "readAnyDatabase");
        ASSERT_EQ(roles[1].getDB(), "admin");
    }
}

TEST(SSLManager, TLSFeatureParser) {
    {
        // test correct feature resolution with one feature
        unsigned char derData[] = {0x30, 0x03, 0x02, 0x01, 0x05};
        std::vector<DERInteger> correctFeatures = {{0x05}};
        auto swFeatures = parseTLSFeature(ConstDataRange(derData));
        ASSERT_OK(swFeatures.getStatus());

        auto features = swFeatures.getValue();
        ASSERT_TRUE(features == correctFeatures);
    }

    {
        // test incorrect feature resolution (malformed header)
        unsigned char derData[] = {0xFF, 0x03, 0x02, 0x01, 0x05};
        std::vector<DERInteger> correctFeatures = {{0x05}};
        auto swFeatures = parseTLSFeature(ConstDataRange(derData));
        ASSERT_NOT_OK(swFeatures.getStatus());
    }

    {
        // test feature resolution with multiple features
        unsigned char derData[] = {0x30, 0x06, 0x02, 0x01, 0x05, 0x02, 0x01, 0x01};
        std::vector<DERInteger> correctFeatures = {{0x05}, {0x01}};
        auto swFeatures = parseTLSFeature(ConstDataRange(derData));
        ASSERT_OK(swFeatures.getStatus());

        auto features = swFeatures.getValue();
        ASSERT_TRUE(features == correctFeatures);
    }
}

TEST(SSLManager, EscapeRFC2253) {
    ASSERT_EQ(escapeRfc2253("abc"), "abc");
    ASSERT_EQ(escapeRfc2253(" abc"), "\\ abc");
    ASSERT_EQ(escapeRfc2253("#abc"), "\\#abc");
    ASSERT_EQ(escapeRfc2253("a,c"), "a\\,c");
    ASSERT_EQ(escapeRfc2253("a+c"), "a\\+c");
    ASSERT_EQ(escapeRfc2253("a\"c"), "a\\\"c");
    ASSERT_EQ(escapeRfc2253("a\\c"), "a\\\\c");
    ASSERT_EQ(escapeRfc2253("a<c"), "a\\<c");
    ASSERT_EQ(escapeRfc2253("a>c"), "a\\>c");
    ASSERT_EQ(escapeRfc2253("a;c"), "a\\;c");
    ASSERT_EQ(escapeRfc2253("abc "), "abc\\ ");
}

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
TEST(SSLManager, DHCheckRFC7919) {
    auto dhparams = makeDefaultDHParameters();
    ASSERT_EQ(verifyDHParameters(dhparams), 0);
}
#endif

struct FlattenedX509Name {
    using EntryVector = std::vector<std::pair<std::string, std::string>>;

    FlattenedX509Name(std::initializer_list<EntryVector::value_type> forVector)
        : value(forVector) {}

    FlattenedX509Name() = default;

    void addPair(std::string oid, std::string val) {
        value.emplace_back(std::move(oid), std::move(val));
    }

    std::string toString() const {
        bool first = true;
        StringBuilder sb;
        for (const auto& entry : value) {
            sb << (first ? "\"" : ",\"") << entry.first << "\"=\"" << entry.second << "\"";
            first = false;
        }

        return sb.str();
    }

    EntryVector value;

    bool operator==(const FlattenedX509Name& other) const {
        return value == other.value;
    }
};

std::ostream& operator<<(std::ostream& o, const FlattenedX509Name& name) {
    o << name.toString();
    return o;
}

FlattenedX509Name flattenX509Name(const SSLX509Name& name) {
    FlattenedX509Name ret;
    for (const auto& entry : name.entries()) {
        for (const auto& rdn : entry) {
            ret.addPair(rdn.oid, rdn.value);
        }
    }

    return ret;
}

TEST(SSLManager, FilterClusterDN) {
    static const stdx::unordered_set<std::string> defaultMatchingAttributes = {
        "0.9.2342.19200300.100.1.25",  // DC
        "2.5.4.10",                    // O
        "2.5.4.11",                    // OU
    };
    std::vector<std::pair<std::string, std::string>> tests = {
        // Single-valued RDNs.
        {"CN=server,OU=Kernel,O=MongoDB,DC=example,L=New York City,ST=New York,C=US",
         "OU=Kernel,O=MongoDB,DC=example"},
        // Multi-valued RDN.
        {"CN=server+OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US", "OU=Kernel,O=MongoDB"},
        // Multiple DC attributes.
        {"CN=server,OU=Kernel,O=MongoDB,DC=example,DC=net,L=New York City,ST=New York,C=US",
         "OU=Kernel,O=MongoDB,DC=example,DC=net"},
    };

    for (const auto& test : tests) {
        LOGV2(7498900, "Testing DN: ", "test_first"_attr = test.first);
        auto swUnfilteredDN = parseDN(test.first);
        auto swExpectedFilteredDN = parseDN(test.second);

        ASSERT_OK(swUnfilteredDN.getStatus());
        ASSERT_OK(swExpectedFilteredDN.getStatus());
        ASSERT_OK(swUnfilteredDN.getValue().normalizeStrings());
        ASSERT_OK(swExpectedFilteredDN.getValue().normalizeStrings());

        auto actualFilteredDN =
            filterClusterDN(swUnfilteredDN.getValue(), defaultMatchingAttributes);
        ASSERT_TRUE(actualFilteredDN == swExpectedFilteredDN.getValue());
    }
};

TEST(SSLManager, DNContains) {
    // Checks if the second RDN is contained by the first (order does not matter).
    // The bool is the expected value.
    std::vector<std::tuple<std::string, std::string, bool>> tests = {
        // Single-valued RDNs positive case.
        {"CN=server,OU=Kernel,O=MongoDB,DC=example,L=New York City,ST=New York,C=US",
         "CN=server,L=New York City,ST=New York,C=US",
         true},
        // Single-valued RDNs mismatched value.
        {"CN=server,OU=Kernel,O=MongoDB,DC=example,L=New York City,ST=New York,C=US",
         "CN=server,L=Yonkers,ST=New York,C=US",
         false},
        // Single-valued RDNs missing attribute.
        {"CN=server,OU=Kernel,O=MongoDB,DC=example,ST=New York,C=US",
         "CN=server,L=Yonkers,ST=New York,C=US",
         false},
        // Multi-valued RDN negative case (attribute value mismatch).
        {"CN=server,OU=Kernel,O=MongoDB,L=New York City+ST=New York,C=US",
         "CN=server,L=Yonkers+ST=New York",
         false},
        // Multi-valued RDN negative case (matching attributes in single-value RDNs, first RDN needs
        // to be filtered beforehand).
        {"CN=server,OU=Kernel,O=MongoDB,L=New York City+ST=New York,C=US",
         "CN=server,L=New York City",
         false},
        // Multi-valued RDN negative case (input DN has attributes in single-value RDNs while match
        // expects multi-valued RDN).
        {"CN=server,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US",
         "CN=server,L=New York City+ST=New York",
         false},
        // Multi-valued RDN positive case (full multi-valued RDN present in second).
        {"CN=server,OU=Kernel,O=MongoDB,L=New York City+ST=New York,C=US",
         "CN=server,L=New York City+ST=New York",
         true},
        // Multiple attributes positive case (order should not matter).
        {"CN=server,OU=Kernel,O=MongoDB,DC=net,DC=example,L=New York City,ST=New York,C=US",
         "OU=Kernel,O=MongoDB,DC=example,DC=net",
         true},
        // Multiple attributes positive case (missing in second, but should not matter).
        {"CN=server,OU=Kernel,O=MongoDB,DC=example,DC=net,L=New York City,ST=New York,C=US",
         "OU=Kernel,O=MongoDB,DC=example",
         true},
        // Multiple attributes negative case (missing in first).
        {"CN=server,OU=Kernel,O=MongoDB,DC=example,L=New York City,ST=New York,C=US",
         "OU=Kernel,O=MongoDB,DC=example,DC=net",
         false},
    };

    for (const auto& test : tests) {
        LOGV2(7498901, "Testing DN: ", "test_first"_attr = std::get<0>(test));
        auto swExternalDN = parseDN(std::get<0>(test));
        auto swMatchPatternDN = parseDN(std::get<1>(test));

        ASSERT_OK(swExternalDN.getStatus());
        ASSERT_OK(swMatchPatternDN.getStatus());

        auto externalDN = swExternalDN.getValue();
        auto matchPatternDN = swMatchPatternDN.getValue();

        ASSERT_OK(externalDN.normalizeStrings());
        ASSERT_OK(matchPatternDN.normalizeStrings());

        ASSERT_EQ(externalDN.contains(matchPatternDN), std::get<2>(test));
    }
};

TEST(SSLManager, DNParsingAndNormalization) {
    std::vector<std::pair<std::string, FlattenedX509Name>> tests = {
        // Basic DN parsing
        {"UID=jsmith,DC=example,DC=net",
         {{"0.9.2342.19200300.100.1.1", "jsmith"},
          {"0.9.2342.19200300.100.1.25", "example"},
          {"0.9.2342.19200300.100.1.25", "net"}}},
        {"OU=Sales+CN=J.  Smith,DC=example,DC=net",
         {{"2.5.4.11", "Sales"},
          {"2.5.4.3", "J.  Smith"},
          {"0.9.2342.19200300.100.1.25", "example"},
          {"0.9.2342.19200300.100.1.25", "net"}}},
        {"CN=server, O=, DC=example, DC=net",
         {{"2.5.4.3", "server"},
          {"2.5.4.10", ""},
          {"0.9.2342.19200300.100.1.25", "example"},
          {"0.9.2342.19200300.100.1.25", "net"}}},
        {R"(CN=James \"Jim\" Smith\, III,DC=example,DC=net)",
         {{"2.5.4.3", R"(James "Jim" Smith, III)"},
          {"0.9.2342.19200300.100.1.25", "example"},
          {"0.9.2342.19200300.100.1.25", "net"}}},
        // Per RFC4518, control sequences are mapped to nothing and whitepace is mapped to ' '
        {"CN=Before\\0aAfter,O=tabs\tare\tspaces\u200B,DC=\\07\\08example,DC=net",
         {{"2.5.4.3", "Before After"},
          {"2.5.4.10", "tabs are spaces"},
          {"0.9.2342.19200300.100.1.25", "example"},
          {"0.9.2342.19200300.100.1.25", "net"}}},
        // Check that you can't fake a cluster dn with poor comma escaping
        {R"(CN=evil\,OU\=Kernel,O=MongoDB Inc.,L=New York City,ST=New York,C=US)",
         {{"2.5.4.3", "evil,OU=Kernel"},
          {"2.5.4.10", "MongoDB Inc."},
          {"2.5.4.7", "New York City"},
          {"2.5.4.8", "New York"},
          {"2.5.4.6", "US"}}},
        // check space handling (must be escaped at the beginning and end of strings)
        {R"(CN= \ escaped spaces\20\  )", {{"2.5.4.3", " escaped spaces  "}}},
        {"CN=server, O=MongoDB Inc.", {{"2.5.4.3", "server"}, {"2.5.4.10", "MongoDB Inc."}}},
        // Check that escaped #'s work correctly at the beginning of the string and throughout.
        {R"(CN=\#1 = \\#1)", {{"2.5.4.3", "#1 = \\#1"}}},
        {R"(CN== \#1)", {{"2.5.4.3", "= #1"}}},
        // check that escaped utf8 string properly parse to utf8
        {R"(CN=Lu\C4\8Di\C4\87)", {{"2.5.4.3", "Lučić"}}},
        // check that unescaped utf8 strings round trip correctly
        {"CN = Калоян, O=مُنظّمة الدُّول المُصدِّرة للنّفْط, L=大田区\\, 東京都",
         {{"2.5.4.3", "Калоян"},
          {"2.5.4.10", "مُنظّمة الدُّول المُصدِّرة للنّفْط"},
          {"2.5.4.7", "大田区, 東京都"}}}};

    for (const auto& test : tests) {
        LOGV2(23268, "Testing DN \"{test_first}\"", "test_first"_attr = test.first);
        auto swDN = parseDN(test.first);
        ASSERT_OK(swDN.getStatus());
        ASSERT_OK(swDN.getValue().normalizeStrings());
        auto decoded = flattenX509Name(swDN.getValue());
        ASSERT_EQ(decoded, test.second);
    }
}

TEST(SSLManager, BadDNParsing) {
    std::vector<std::string> tests = {"CN=#12345", R"(CN=\B)", R"(CN=<", "\)"};
    for (const auto& test : tests) {
        LOGV2(23269, "Testing bad DN: \"{test}\"", "test"_attr = test);
        auto swDN = parseDN(test);
        ASSERT_NOT_OK(swDN.getStatus());
    }
}

TEST(SSLManager, RotateCertificatesFromFile) {
    SSLParams params;
    params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
    // Server is required to have the sslPEMKeyFile.
    params.sslPEMKeyFile = "jstests/libs/server.pem";
    params.sslCAFile = "jstests/libs/ca.pem";
    params.sslClusterFile = "jstests/libs/client.pem";

    std::shared_ptr<SSLManagerInterface> manager =
        SSLManagerInterface::create(params, true /* isSSLServer */);

    ServiceEntryPointUtil sepu;

    auto options = [] {
        ServerGlobalParams params;
        params.noUnixSocket = true;
        transport::AsioTransportLayer::Options opts(&params);
        return opts;
    }();
    transport::AsioTransportLayer tla(options, &sepu);
    uassertStatusOK(tla.rotateCertificates(manager, false /* asyncOCSPStaple */));
}

TEST(SSLManager, InitContextFromFileShouldFail) {
    SSLParams params;
    params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
    // Server is required to have the sslPEMKeyFile.
    // We force the initialization to fail by omitting this param.
    params.sslCAFile = "jstests/libs/ca.pem";
    params.sslClusterFile = "jstests/libs/client.pem";
#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
    ASSERT_THROWS_CODE(
        [&params] {
            SSLManagerInterface::create(params, true /* isSSLServer */);
        }(),
        DBException,
        ErrorCodes::InvalidSSLConfiguration);
#endif
}

TEST(SSLManager, RotateClusterCertificatesFromFile) {
    SSLParams params;
    params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
    // Client doesn't need params.sslPEMKeyFile.
    params.sslCAFile = "jstests/libs/ca.pem";
    params.sslClusterFile = "jstests/libs/client.pem";

    std::shared_ptr<SSLManagerInterface> manager =
        SSLManagerInterface::create(params, false /* isSSLServer */);

    ServiceEntryPointUtil sepu;

    auto options = [] {
        ServerGlobalParams params;
        params.noUnixSocket = true;
        transport::AsioTransportLayer::Options opts(&params);
        return opts;
    }();
    transport::AsioTransportLayer tla(options, &sepu);
    uassertStatusOK(tla.rotateCertificates(manager, false /* asyncOCSPStaple */));
}

#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL

TEST(SSLManager, InitContextFromFile) {
    SSLParams params;
    params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
    // Client doesn't need params.sslPEMKeyFile.
    params.sslClusterFile = "jstests/libs/client.pem";

    std::shared_ptr<SSLManagerInterface> manager =
        SSLManagerInterface::create(params, false /* isSSLServer */);

    auto egress = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
    uassertStatusOK(manager->initSSLContext(
        egress->native_handle(), params, SSLManagerInterface::ConnectionDirection::kOutgoing));
}

TEST(SSLManager, InitContextFromMemory) {
    SSLParams params;
    params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
    params.sslCAFile = "jstests/libs/ca.pem";

    TransientSSLParams transientParams;
    transientParams.sslClusterPEMPayload = loadFile("jstests/libs/client.pem");

    std::shared_ptr<SSLManagerInterface> manager =
        SSLManagerInterface::create(params, transientParams, false /* isSSLServer */);

    auto egress = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
    uassertStatusOK(manager->initSSLContext(
        egress->native_handle(), params, SSLManagerInterface::ConnectionDirection::kOutgoing));
}

// Tests when 'is server' param to managed interface creation is set, it is ignored.
TEST(SSLManager, IgnoreInitServerSideContextFromMemory) {
    SSLParams params;
    params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
    params.sslPEMKeyFile = "jstests/libs/server.pem";
    params.sslCAFile = "jstests/libs/ca.pem";

    TransientSSLParams transientParams;
    transientParams.sslClusterPEMPayload = loadFile("jstests/libs/client.pem");

    std::shared_ptr<SSLManagerInterface> manager =
        SSLManagerInterface::create(params, transientParams, true /* isSSLServer */);

    auto egress = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
    uassertStatusOK(manager->initSSLContext(
        egress->native_handle(), params, SSLManagerInterface::ConnectionDirection::kOutgoing));
}

TEST(SSLManager, TransientSSLParams) {
    SSLParams params;
    params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
    params.sslCAFile = "jstests/libs/ca.pem";
    params.sslClusterFile = "jstests/libs/client.pem";

    ServiceEntryPointUtil sepu;

    auto options = [] {
        ServerGlobalParams params;
        params.noUnixSocket = true;
        transport::AsioTransportLayer::Options opts(&params);
        return opts;
    }();
    transport::AsioTransportLayer tla(options, &sepu);

    TransientSSLParams transientSSLParams;
    transientSSLParams.sslClusterPEMPayload = loadFile("jstests/libs/client.pem");
    transientSSLParams.targetedClusterConnectionString = ConnectionString::forLocal();

    auto swContext = tla.createTransientSSLContext(transientSSLParams);
    uassertStatusOK(swContext.getStatus());

    // Check that the manager owned by the transient context is also transient.
    ASSERT_TRUE(swContext.getValue()->manager->isTransient());
    ASSERT_EQ(transientSSLParams.targetedClusterConnectionString.toString(),
              swContext.getValue()->manager->getTargetedClusterConnectionString());

    // Cannot rotate certs on transient manager.
    ASSERT_NOT_OK(tla.rotateCertificates(swContext.getValue()->manager, true));
}

TEST(SSLManager, TransientSSLParamsStressTestWithTransport) {
    static constexpr int kMaxContexts = 100;
    static constexpr int kThreads = 10;
    SSLParams params;
    params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
    params.sslCAFile = "jstests/libs/ca.pem";

    ServiceEntryPointUtil sepu;

    auto options = [] {
        ServerGlobalParams params;
        params.noUnixSocket = true;
        transport::AsioTransportLayer::Options opts(&params);
        return opts;
    }();
    transport::AsioTransportLayer tla(options, &sepu);

    TransientSSLParams transientSSLParams;
    transientSSLParams.sslClusterPEMPayload = loadFile("jstests/libs/client.pem");
    transientSSLParams.targetedClusterConnectionString = ConnectionString::forLocal();

    Mutex mutex = MONGO_MAKE_LATCH("::test_mutex");
    std::deque<std::shared_ptr<const transport::SSLConnectionContext>> contexts;
    std::vector<stdx::thread> threads;
    Counter64 iterations;

    for (int t = 0; t < kThreads; ++t) {
        stdx::thread thread([&]() {
            Timer timer;
            while (timer.elapsed() < Seconds(2)) {
                auto swContext = tla.createTransientSSLContext(transientSSLParams);
                invariant(swContext.getStatus().isOK());
                std::shared_ptr<const transport::SSLConnectionContext> ctxToDelete;
                {
                    auto lk = stdx::lock_guard(mutex);
                    contexts.push_back(std::move(swContext.getValue()));
                    if (contexts.size() > kMaxContexts) {
                        ctxToDelete = contexts.front();
                        contexts.pop_front();
                    }
                }
                iterations.increment();
            }
        });
        threads.push_back(std::move(thread));
    }
    for (auto& t : threads) {
        t.join();
    }

    contexts.clear();
    LOGV2(5906701, "Stress test completed", "iterations"_attr = iterations);
}

TEST(SSLManager, TransientSSLParamsStressTestWithManager) {
    static constexpr int kMaxManagers = 100;
    static constexpr int kThreads = 10;
    SSLParams params;
    params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
    params.sslPEMKeyFile = "jstests/libs/server.pem";
    params.sslCAFile = "jstests/libs/ca.pem";

    TransientSSLParams transientParams;
    transientParams.sslClusterPEMPayload = loadFile("jstests/libs/client.pem");

    Mutex mutex = MONGO_MAKE_LATCH("::test_mutex");
    std::deque<std::shared_ptr<SSLManagerInterface>> managers;
    std::vector<stdx::thread> threads;
    Counter64 iterations;

    for (int t = 0; t < kThreads; ++t) {
        stdx::thread thread([&]() {
            Timer timer;
            while (timer.elapsed() < Seconds(3)) {
                std::shared_ptr<SSLManagerInterface> manager =
                    SSLManagerInterface::create(params, transientParams, true /* isSSLServer */);

                auto egress = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
                invariant(manager
                              ->initSSLContext(egress->native_handle(),
                                               params,
                                               SSLManagerInterface::ConnectionDirection::kOutgoing)
                              .isOK());
                std::shared_ptr<SSLManagerInterface> managerToDelete;
                {
                    auto lk = stdx::lock_guard(mutex);
                    managers.push_back(std::move(manager));
                    if (managers.size() > kMaxManagers) {
                        managerToDelete = managers.front();
                        managers.pop_front();
                    }
                }
                iterations.increment();
            }
        });
        threads.push_back(std::move(thread));
    }
    for (auto& t : threads) {
        t.join();
    }

    managers.clear();
    LOGV2(5906702, "Stress test completed", "iterations"_attr = iterations);
}

#endif  // MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL

static bool isSanWarningWritten(const std::vector<std::string>& logLines) {
    for (const auto& line : logLines) {
        if (std::string::npos !=
            line.find("Server certificate has no compatible Subject Alternative Name")) {
            return true;
        }
    }
    return false;
}

// This test verifies there is a startup warning if Subject Alternative Name is missing
TEST(SSLManager, InitContextSanWarning) {
    SSLParams params;
    params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
    params.sslCAFile = "jstests/libs/ca.pem";
    params.sslPEMKeyFile = "jstests/libs/server_no_SAN.pem";

    startCapturingLogMessages();
    auto manager = SSLManagerInterface::create(params, true);
    auto egress = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);

    uassertStatusOK(manager->initSSLContext(
        egress->native_handle(), params, SSLManagerInterface::ConnectionDirection::kIncoming));
    stopCapturingLogMessages();

    ASSERT_TRUE(isSanWarningWritten(getCapturedTextFormatLogMessages()));
}

// This test verifies there is no startup warning if Subject Alternative Name is present
TEST(SSLManager, InitContextNoSanWarning) {
    SSLParams params;
    params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
    params.sslCAFile = "jstests/libs/ca.pem";
    params.sslPEMKeyFile = "jstests/libs/server.pem";

    startCapturingLogMessages();
    auto manager = SSLManagerInterface::create(params, true);
    auto egress = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);

    uassertStatusOK(manager->initSSLContext(
        egress->native_handle(), params, SSLManagerInterface::ConnectionDirection::kIncoming));
    stopCapturingLogMessages();

    ASSERT_FALSE(isSanWarningWritten(getCapturedTextFormatLogMessages()));
}


}  // namespace
}  // namespace mongo
