/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/rpc/metadata/audit_metadata.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/metadata/audit_client_attrs.h"
#include "mongo/rpc/metadata/audit_user_attrs.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/net/hostandport.h"

namespace mongo::rpc {
namespace {

constexpr auto kLocalAddr = "127.0.0.1:2000"_sd;
constexpr auto kRemoteAddr = "127.0.0.2:27018"_sd;
constexpr auto kProxyAddr = "10.0.0.1:8080"_sd;
constexpr std::array<StringData, 2> kExpectedProxies = {kLocalAddr, kProxyAddr};

constexpr auto kUserName = "testUser"_sd;
constexpr auto kDBName = "admin"_sd;
constexpr auto kRoleName = "root"_sd;

class AuditMetadataTest : public ServiceContextTest {
protected:
    void setUp() override {
        ServiceContextTest::setUp();
        auto session = transport::MockSession::create(&_transportLayer);

        Client::releaseCurrent();
        auto client = getService()->makeClient("AuditMetadataTestClient", session);
        _client = client.get();
        Client::setCurrent(std::move(client));

        _opCtxPtr = _client->makeOperationContext();
        ASSERT_OK(ServerParameterSet::getNodeParameterSet()
                      ->get("featureFlagExposeClientIpInAuditLogs")
                      ->setFromString("true", boost::none));
    }

    OperationContext* opCtx() const {
        return _opCtxPtr.get();
    }

    Client* client() const {
        return _client;
    }

    void setUpTestData() {
        auto local = HostAndPort(kLocalAddr);
        auto remote = HostAndPort(kRemoteAddr);
        std::vector<HostAndPort> proxies{HostAndPort(kProxyAddr)};
        auto userName = UserName(kUserName, kDBName);
        std::vector<RoleName> roleNames{RoleName{kRoleName, kDBName}};

        AuditClientAttrs::set(_client,
                              AuditClientAttrs(std::move(local),
                                               std::move(remote),
                                               std::move(proxies),
                                               true /* isImpersonating */));
        AuditUserAttrs::set(opCtx(), userName, roleNames, true /* isImpersonating */);
    }

private:
    transport::TransportLayerMock _transportLayer;
    ServiceContext::UniqueOperationContext _opCtxPtr;
    Client* _client;
};

TEST_F(AuditMetadataTest, GetAuthDataToAuditMetadataEmpty) {
    auto result = getAuditAttrsToAuditMetadata(nullptr);
    ASSERT_FALSE(result.has_value());

    result = getAuditAttrsToAuditMetadata(opCtx());
    ASSERT_FALSE(result.has_value());
}

TEST_F(AuditMetadataTest, GetAuthDataToAuditMetadata) {
    setUpTestData();

    boost::optional<AuditMetadata> result = getAuditAttrsToAuditMetadata(opCtx());

    auto impersonatedUser = result->getUser();
    ASSERT_EQUALS(kUserName, impersonatedUser->getUser());
    ASSERT_EQUALS(kDBName, impersonatedUser->getDatabaseName().toString_forTest());

    auto impersonatedRoles = result->getRoles();
    ASSERT_EQUALS(1, impersonatedRoles.size());
    ASSERT_EQUALS(kRoleName, impersonatedRoles[0].getRole());
    ASSERT_EQUALS(kDBName, impersonatedRoles[0].getDatabaseName().toString_forTest());

    auto impersonatedClient = result->getClientMetadata();
    auto hosts = impersonatedClient->getHosts();
    ASSERT_EQUALS(3, hosts.size());
    ASSERT_EQUALS(kRemoteAddr, hosts[0].toString());
    ASSERT_EQUALS(kLocalAddr, hosts[1].toString());
    ASSERT_EQUALS(kProxyAddr, hosts[2].toString());
}

TEST_F(AuditMetadataTest, ReadAuditMetadata) {
    auto auditClientAttrs = rpc::AuditClientAttrs::get(client());
    auto auditUserAttrs = rpc::AuditUserAttrs::get(opCtx());
    ASSERT_FALSE(auditClientAttrs);
    ASSERT_FALSE(auditUserAttrs);

    // Construct an AuditMetadata object representing a parsed $audit object.
    BSONObj dollarAudit =
        BSON("$impersonatedUser" << BSON("user" << kUserName << "db" << kDBName)
                                 << "$impersonatedRoles" << BSONArray() << "$impersonatedClient"
                                 << BSON("hosts"
                                         << BSON_ARRAY(kRemoteAddr << kLocalAddr << kProxyAddr)));
    AuditMetadata parsedDollarAudit =
        AuditMetadata::parse(IDLParserContext{kImpersonationMetadataSectionName}, dollarAudit);
    GenericArguments requestArgs;
    requestArgs.setDollarAudit(parsedDollarAudit);

    {
        boost::optional<rpc::ImpersonatedClientSessionGuard> clientSessionGuard;
        ASSERT_FALSE(clientSessionGuard);
        rpc::readRequestMetadata(opCtx(), requestArgs, false, clientSessionGuard);
        ASSERT_TRUE(clientSessionGuard.has_value());

        // Now, AuditClientAttrs and AuditUserAttrs should be updated to store the users and client
        // info supplied by requestArgs.
        auditClientAttrs = rpc::AuditClientAttrs::get(client());
        auditUserAttrs = rpc::AuditUserAttrs::get(opCtx());
        ASSERT_TRUE(auditClientAttrs);
        ASSERT_TRUE(auditUserAttrs);

        ASSERT_EQ(auditUserAttrs->getUser().getUser(), kUserName);
        ASSERT_EQ(auditUserAttrs->getUser().getDatabaseName().toString_forTest(), kDBName);

        ASSERT_EQ(auditClientAttrs->getRemote(), HostAndPort::parse(kRemoteAddr));
        ASSERT_EQ(kExpectedProxies.size(), auditClientAttrs->getProxies().size());
        for (size_t i = 0; i < auditClientAttrs->getProxies().size(); i++) {
            ASSERT_EQ(auditClientAttrs->getProxies()[i], HostAndPort::parse(kExpectedProxies[i]));
        }
    }

    // After clientSessionGuard goes out of scope, the AuditClientAttrs should be cleared. Since
    // AuditUserAttrs is scoped to a single OperationContext and that is still alive, it will be
    // nonempty.
    auditClientAttrs = rpc::AuditClientAttrs::get(client());
    auditUserAttrs = rpc::AuditUserAttrs::get(opCtx());
    ASSERT_FALSE(auditClientAttrs);
    ASSERT_TRUE(auditUserAttrs);
}

TEST_F(AuditMetadataTest, ReadAuditMetadataUserOnly) {
    auto auditClientAttrs = rpc::AuditClientAttrs::get(client());
    auto auditUserAttrs = rpc::AuditUserAttrs::get(opCtx());
    ASSERT_FALSE(auditClientAttrs);
    ASSERT_FALSE(auditUserAttrs);

    // Construct an AuditMetadata object representing a parsed $audit object.
    BSONObj dollarAudit = BSON("$impersonatedUser" << BSON("user" << kUserName << "db" << kDBName)
                                                   << "$impersonatedRoles" << BSONArray());
    AuditMetadata parsedDollarAudit =
        AuditMetadata::parse(IDLParserContext{kImpersonationMetadataSectionName}, dollarAudit);
    GenericArguments requestArgs;
    requestArgs.setDollarAudit(parsedDollarAudit);

    {
        boost::optional<rpc::ImpersonatedClientSessionGuard> clientSessionGuard;
        ASSERT_FALSE(clientSessionGuard);
        rpc::readRequestMetadata(opCtx(), requestArgs, false, clientSessionGuard);
        ASSERT_FALSE(clientSessionGuard.has_value());

        // Now, AuditClientAttrs and AuditUserAttrs should be updated to store the user
        // info supplied by requestArgs. Since there was nothing in $impersonatedClient,
        // auditClientAttrs should still be empty.
        auditClientAttrs = rpc::AuditClientAttrs::get(client());
        auditUserAttrs = rpc::AuditUserAttrs::get(opCtx());
        ASSERT_FALSE(auditClientAttrs);
        ASSERT_TRUE(auditUserAttrs);

        ASSERT_EQ(auditUserAttrs->getUser().getUser(), kUserName);
        ASSERT_EQ(auditUserAttrs->getUser().getDatabaseName().toString_forTest(), kDBName);
    }

    // After clientSessionGuard goes out of scope, the AuditClientAttrs should be cleared. Since
    // AuditUserAttrs is scoped to a single OperationContext and that is still alive, it will be
    // nonempty.
    auditClientAttrs = rpc::AuditClientAttrs::get(client());
    auditUserAttrs = rpc::AuditUserAttrs::get(opCtx());
    ASSERT_FALSE(auditClientAttrs);
    ASSERT_TRUE(auditUserAttrs);
}

TEST_F(AuditMetadataTest, ReadAuditMetadataClientOnly) {
    auto auditClientAttrs = rpc::AuditClientAttrs::get(client());
    auto auditUserAttrs = rpc::AuditUserAttrs::get(opCtx());
    ASSERT_FALSE(auditClientAttrs);
    ASSERT_FALSE(auditUserAttrs);

    // Construct an AuditMetadata object representing a parsed $audit object.
    BSONObj dollarAudit =
        BSON("$impersonatedRoles" << BSONArray() << "$impersonatedClient"
                                  << BSON("hosts"
                                          << BSON_ARRAY(kRemoteAddr << kLocalAddr << kProxyAddr)));
    AuditMetadata parsedDollarAudit =
        AuditMetadata::parse(IDLParserContext{kImpersonationMetadataSectionName}, dollarAudit);
    GenericArguments requestArgs;
    requestArgs.setDollarAudit(parsedDollarAudit);

    {
        boost::optional<rpc::ImpersonatedClientSessionGuard> clientSessionGuard;
        ASSERT_FALSE(clientSessionGuard);
        rpc::readRequestMetadata(opCtx(), requestArgs, false, clientSessionGuard);
        ASSERT_TRUE(clientSessionGuard.has_value());

        // Now, AuditClientAttrs should be updated to store the client
        // info supplied by requestArgs.
        auditClientAttrs = rpc::AuditClientAttrs::get(client());
        auditUserAttrs = rpc::AuditUserAttrs::get(opCtx());
        ASSERT_TRUE(auditClientAttrs);
        ASSERT_FALSE(auditUserAttrs);

        ASSERT_EQ(auditClientAttrs->getRemote(), HostAndPort::parse(kRemoteAddr));
        ASSERT_EQ(kExpectedProxies.size(), auditClientAttrs->getProxies().size());
        for (size_t i = 0; i < auditClientAttrs->getProxies().size(); i++) {
            ASSERT_EQ(auditClientAttrs->getProxies()[i], HostAndPort::parse(kExpectedProxies[i]));
        }
    }

    // After clientSessionGuard goes out of scope, the AuditClientAttrs should be cleared.
    auditClientAttrs = rpc::AuditClientAttrs::get(client());
    auditUserAttrs = rpc::AuditUserAttrs::get(opCtx());
    ASSERT_FALSE(auditClientAttrs);
    ASSERT_FALSE(auditUserAttrs);
}

TEST_F(AuditMetadataTest, WriteAuditMetadata) {
    setUpTestData();

    BSONObjBuilder builder;
    writeAuditMetadata(opCtx(), &builder);
    auto result = builder.obj();

    ASSERT(result.hasField(kImpersonationMetadataSectionName));
    auto metadata = result.getObjectField(kImpersonationMetadataSectionName);

    auto impersonatedUser = metadata.getObjectField("$impersonatedUser");
    ASSERT_EQUALS(kUserName, impersonatedUser.getStringField("user"));
    ASSERT_EQUALS(kDBName, impersonatedUser.getStringField("db"));

    auto impersonatedRoles = metadata.getField("$impersonatedRoles").Array();
    ASSERT_EQUALS(1, impersonatedRoles.size());
    auto roleObj = impersonatedRoles[0].Obj();
    ASSERT_EQUALS(kRoleName, roleObj.getStringField("role"));
    ASSERT_EQUALS(kDBName, roleObj.getStringField("db"));

    auto impersonatedClient = metadata.getObjectField("$impersonatedClient");
    auto hosts = impersonatedClient.getField("hosts").Array();
    ASSERT_EQUALS(3, hosts.size());
    ASSERT_EQUALS(kRemoteAddr, hosts[0].String());
    ASSERT_EQUALS(kLocalAddr, hosts[1].String());
    ASSERT_EQUALS(kProxyAddr, hosts[2].String());
}

TEST_F(AuditMetadataTest, WriteAuditMetadataDisableFF) {
    // Turning off feature flag, we expect the finalized object of $audit to NOT contain the
    // $impersonatedClient field.
    ASSERT_OK(ServerParameterSet::getNodeParameterSet()
                  ->get("featureFlagExposeClientIpInAuditLogs")
                  ->setFromString("false", boost::none));

    setUpTestData();

    BSONObjBuilder builder;
    writeAuditMetadata(opCtx(), &builder);
    auto result = builder.obj();

    ASSERT(result.hasField(kImpersonationMetadataSectionName));
    auto metadata = result.getObjectField(kImpersonationMetadataSectionName);

    auto impersonatedUser = metadata.getObjectField("$impersonatedUser");
    ASSERT_EQUALS(kUserName, impersonatedUser.getStringField("user"));
    ASSERT_EQUALS(kDBName, impersonatedUser.getStringField("db"));

    auto impersonatedRoles = metadata.getField("$impersonatedRoles").Array();
    ASSERT_EQUALS(1, impersonatedRoles.size());
    auto roleObj = impersonatedRoles[0].Obj();
    ASSERT_EQUALS(kRoleName, roleObj.getStringField("role"));
    ASSERT_EQUALS(kDBName, roleObj.getStringField("db"));

    ASSERT(!metadata.hasField("$impersonatedClient"));
}

}  // namespace
}  // namespace mongo::rpc
