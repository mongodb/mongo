/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/transport/mock_session.h"
#include "mongo/transport/proxy_protocol_header_parser.h"
#include "mongo/transport/proxy_protocol_tlv_extraction.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport {
namespace {

/**
 * Exercises the TLV extraction logic used in asio_session_impl.cpp to populate SSLPeerInfo from
 * proxy protocol ParserResults. The logic under test:
 *   - Extracts SNI from top-level TLVs (kProxyProtocolTypeAuthority)
 *   - Extracts DN from SSL sub-TLVs (kProxyProtocolSSLTlvDN)
 *   - Extracts roles from SSL sub-TLVs (kProxyProtocolSSLTlvPeerRoles)
 *   - Sets SSLPeerInfo on the session only when at least one field is present
 */

// DER-encoded role data for a single role: role="role_name", db="Third field"
// From ssl_manager_test.cpp
const unsigned char kSingleRoleDer[] = {0x31, 0x1a, 0x30, 0x18, 0x0c, 0x09, 0x72, 0x6f, 0x6c, 0x65,
                                        0x5f, 0x6e, 0x61, 0x6d, 0x65, 0x0c, 0x0b, 0x54, 0x68, 0x69,
                                        0x72, 0x64, 0x20, 0x66, 0x69, 0x65, 0x6c, 0x64};

// Uses applyProxyProtocolTlvs() from proxy_protocol_tlv_extraction.h — the same
// function that asio_session_impl.cpp calls in production.

class ProxyProtocolTlvSSLPeerInfoTest : public unittest::Test {
protected:
    void setUp() override {
        _session = MockSession::create(&_transportLayer);
    }

    std::shared_ptr<Session>& session() {
        return _session;
    }

    std::string singleRoleDerString() {
        return std::string(reinterpret_cast<const char*>(kSingleRoleDer), sizeof(kSingleRoleDer));
    }

private:
    TransportLayerMock _transportLayer;
    std::shared_ptr<Session> _session;
};

TEST_F(ProxyProtocolTlvSSLPeerInfoTest, SNIExtractedFromTopLevelAuthority) {
    ParserResults results;
    results.tlvs.push_back({kProxyProtocolTypeAuthority, "my.mongodb.com"});

    applyProxyProtocolTlvs(results, session());

    auto sslPeerInfo = SSLPeerInfo::forSession(session());
    ASSERT_TRUE(sslPeerInfo);
    ASSERT_TRUE(sslPeerInfo->sniName().has_value());
    ASSERT_EQ(sslPeerInfo->sniName().value(), "my.mongodb.com");
    // No DN or roles should be set.
    ASSERT_TRUE(sslPeerInfo->subjectName().empty());
    ASSERT_TRUE(sslPeerInfo->roles().empty());
}

TEST_F(ProxyProtocolTlvSSLPeerInfoTest, DNExtractedFromSSLSubTLV) {
    ParserResults results;
    results.sslTlvs = ProxiedSSLData{};
    results.sslTlvs->subTLVs.push_back(
        {kProxyProtocolSSLTlvDN, "CN=server,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US"});

    applyProxyProtocolTlvs(results, session());

    auto sslPeerInfo = SSLPeerInfo::forSession(session());
    ASSERT_TRUE(sslPeerInfo);
    ASSERT_FALSE(sslPeerInfo->subjectName().empty());
    // Verify the DN was parsed correctly by checking an OID.
    auto swCN = sslPeerInfo->subjectName().getOID("2.5.4.3");  // CN
    ASSERT_OK(swCN.getStatus());
    ASSERT_EQ(swCN.getValue(), "server");
    // No SNI should be set.
    ASSERT_FALSE(sslPeerInfo->sniName().has_value());
}

TEST_F(ProxyProtocolTlvSSLPeerInfoTest, RolesWithoutDNShouldFail) {
    ParserResults results;
    results.sslTlvs = ProxiedSSLData{};
    results.sslTlvs->subTLVs.push_back({kProxyProtocolSSLTlvPeerRoles, singleRoleDerString()});

    ASSERT_THROWS_CODE(
        applyProxyProtocolTlvs(results, session()), DBException, ErrorCodes::BadValue);
}

TEST_F(ProxyProtocolTlvSSLPeerInfoTest, SNIAndDNAndRolesAllPresent) {
    ParserResults results;
    // Top-level TLV for SNI
    results.tlvs.push_back({kProxyProtocolTypeAuthority, "my.mongodb.com"});
    // SSL sub-TLVs for DN and roles
    results.sslTlvs = ProxiedSSLData{};
    results.sslTlvs->subTLVs.push_back({kProxyProtocolSSLTlvDN, "CN=client,O=MongoDB"});
    results.sslTlvs->subTLVs.push_back({kProxyProtocolSSLTlvPeerRoles, singleRoleDerString()});

    applyProxyProtocolTlvs(results, session());

    auto sslPeerInfo = SSLPeerInfo::forSession(session());
    ASSERT_TRUE(sslPeerInfo);

    // SNI
    ASSERT_TRUE(sslPeerInfo->sniName().has_value());
    ASSERT_EQ(sslPeerInfo->sniName().value(), "my.mongodb.com");

    // DN
    ASSERT_FALSE(sslPeerInfo->subjectName().empty());
    auto swCN = sslPeerInfo->subjectName().getOID("2.5.4.3");
    ASSERT_OK(swCN.getStatus());
    ASSERT_EQ(swCN.getValue(), "client");

    // Roles
    ASSERT_EQ(sslPeerInfo->roles().size(), 1u);
    auto role = *sslPeerInfo->roles().begin();
    ASSERT_EQ(role.getRole(), "role_name");
    ASSERT_EQ(role.getDB(), "Third field");
}

TEST_F(ProxyProtocolTlvSSLPeerInfoTest, SNIFromTopLevelNotSSLSubTLV) {
    // Verify that SNI is correctly read from top-level TLVs, not from SSL sub-TLVs.
    // The Authority TLV (0x02) should only be in top-level TLVs.
    ParserResults results;
    results.tlvs.push_back({kProxyProtocolTypeAuthority, "top-level.mongodb.com"});

    // SSL sub-TLVs present but with no Authority type (Authority is not a valid SSL sub-TLV)
    results.sslTlvs = ProxiedSSLData{};
    results.sslTlvs->subTLVs.push_back({kProxyProtocolSSLTlvDN, "CN=cn.example.com"});

    applyProxyProtocolTlvs(results, session());

    auto sslPeerInfo = SSLPeerInfo::forSession(session());
    ASSERT_TRUE(sslPeerInfo);
    ASSERT_TRUE(sslPeerInfo->sniName().has_value());
    ASSERT_EQ(sslPeerInfo->sniName().value(), "top-level.mongodb.com");
}

TEST_F(ProxyProtocolTlvSSLPeerInfoTest, NoSSLPeerInfoWhenNoTLVs) {
    ParserResults results;

    applyProxyProtocolTlvs(results, session());

    auto sslPeerInfo = SSLPeerInfo::forSession(session());
    ASSERT_FALSE(sslPeerInfo);
}

TEST_F(ProxyProtocolTlvSSLPeerInfoTest, SSLTLVsWithoutRelevantSubTLVsAndNoSNI) {
    // SSL TLVs exist but contain no DN or roles sub-TLVs.
    // And no Authority TLV in top-level TLVs.
    ParserResults results;
    results.sslTlvs = ProxiedSSLData{};

    applyProxyProtocolTlvs(results, session());

    auto sslPeerInfo = SSLPeerInfo::forSession(session());
    ASSERT_FALSE(sslPeerInfo);
}

TEST_F(ProxyProtocolTlvSSLPeerInfoTest, SNIOnlyWithoutSSLTLVs) {
    // Authority TLV in top-level TLVs, but no SSL TLVs at all.
    // SNI alone should trigger SSLPeerInfo creation.
    ParserResults results;
    results.tlvs.push_back({kProxyProtocolTypeAuthority, "standalone-sni.com"});

    applyProxyProtocolTlvs(results, session());

    auto sslPeerInfo = SSLPeerInfo::forSession(session());
    ASSERT_TRUE(sslPeerInfo);
    ASSERT_TRUE(sslPeerInfo->sniName().has_value());
    ASSERT_EQ(sslPeerInfo->sniName().value(), "standalone-sni.com");
    ASSERT_TRUE(sslPeerInfo->subjectName().empty());
    ASSERT_TRUE(sslPeerInfo->roles().empty());
}

TEST_F(ProxyProtocolTlvSSLPeerInfoTest, MultipleTopLevelTLVsFirstAuthorityWins) {
    // If multiple Authority TLVs exist, only the first should be used (break on first match).
    ParserResults results;
    results.tlvs.push_back({kProxyProtocolTypeAuthority, "first.mongodb.com"});
    results.tlvs.push_back({kProxyProtocolTypeAuthority, "second.mongodb.com"});

    applyProxyProtocolTlvs(results, session());

    auto sslPeerInfo = SSLPeerInfo::forSession(session());
    ASSERT_TRUE(sslPeerInfo);
    ASSERT_TRUE(sslPeerInfo->sniName().has_value());
    ASSERT_EQ(sslPeerInfo->sniName().value(), "first.mongodb.com");
}

TEST_F(ProxyProtocolTlvSSLPeerInfoTest, IsTLSFlagNotSetForProxyProtocol) {
    // Proxy protocol connections arrive over UDS (not TLS), so isTLS() should be false
    // even though the original client used TLS. The production code in
    // asio_session_impl.cpp passes isTLS=false to the SSLPeerInfo constructor.
    ParserResults results;
    results.tlvs.push_back({kProxyProtocolTypeAuthority, "tls-flag-check.com"});

    applyProxyProtocolTlvs(results, session());

    auto sslPeerInfo = SSLPeerInfo::forSession(session());
    ASSERT_TRUE(sslPeerInfo);
    ASSERT_FALSE(sslPeerInfo->isTLS());
}

TEST_F(ProxyProtocolTlvSSLPeerInfoTest, DNWithMultipleRDNs) {
    ParserResults results;
    results.sslTlvs = ProxiedSSLData{};
    results.sslTlvs->subTLVs.push_back(
        {kProxyProtocolSSLTlvDN,
         "CN=client.mongodb.com,OU=Kernel,O=MongoDB,L=New York City,ST=New York,C=US"});

    applyProxyProtocolTlvs(results, session());

    auto sslPeerInfo = SSLPeerInfo::forSession(session());
    ASSERT_TRUE(sslPeerInfo);
    ASSERT_FALSE(sslPeerInfo->subjectName().empty());

    // Verify individual OIDs.
    auto swCN = sslPeerInfo->subjectName().getOID("2.5.4.3");  // CN
    ASSERT_OK(swCN.getStatus());
    ASSERT_EQ(swCN.getValue(), "client.mongodb.com");

    auto swO = sslPeerInfo->subjectName().getOID("2.5.4.10");  // O
    ASSERT_OK(swO.getStatus());
    ASSERT_EQ(swO.getValue(), "MongoDB");

    auto swC = sslPeerInfo->subjectName().getOID("2.5.4.6");  // C
    ASSERT_OK(swC.getStatus());
    ASSERT_EQ(swC.getValue(), "US");
}

}  // namespace
}  // namespace mongo::transport
