/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_session_test_fixture.h"

#include <boost/none.hpp>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/assert_util.h"

namespace mongo {

void AuthorizationSessionTestFixture::setUp() {
    gMultitenancySupport = true;
    ServiceContextMongoDTest::setUp();

    _session = transportLayer.createSession();
    _client = getServiceContext()->getService()->makeClient("testClient", _session);
    _opCtx = _client->makeOperationContext();
    managerState->setAuthzVersion(_opCtx.get(), AuthorizationManager::schemaVersion26Final);

    authzManager = AuthorizationManager::get(_client->getService());
    auto localSessionState = std::make_unique<AuthzSessionExternalStateMock>(authzManager);
    sessionState = localSessionState.get();
    authzSession = std::make_unique<AuthorizationSessionForTest>(
        std::move(localSessionState), AuthorizationSessionImpl::InstallMockForTestingOrAuthImpl{});
    authzSession->startContractTracking();

    credentials =
        BSON("SCRAM-SHA-1" << scram::Secrets<SHA1Block>::generateCredentials(
                                  "a", saslGlobalParams.scramSHA1IterationCount.load())
                           << "SCRAM-SHA-256"
                           << scram::Secrets<SHA256Block>::generateCredentials(
                                  "a", saslGlobalParams.scramSHA256IterationCount.load()));

    // assume tenantId is from security token with no expectPrefix
    _sc = SerializationContext(SerializationContext::Source::Command,
                               SerializationContext::CallerType::Request,
                               SerializationContext::Prefix::ExcludePrefix);
}

Status AuthorizationSessionTestFixture::createUser(const UserName& username,
                                                   const std::vector<RoleName>& roles) {
    BSONObjBuilder userDoc;
    userDoc.append("_id", username.getUnambiguousName());
    username.appendToBSON(&userDoc);
    userDoc.append("credentials", credentials);

    BSONArrayBuilder rolesBSON(userDoc.subarrayStart("roles"));
    for (const auto& role : roles) {
        role.serializeToBSON(&rolesBSON);
    }
    rolesBSON.doneFast();

    return managerState->insert(_opCtx.get(),
                                NamespaceString::makeTenantUsersCollection(username.getTenant()),
                                userDoc.obj(),
                                {});
}

void AuthorizationSessionTestFixture::assertLogout(const ResourcePattern& resource,
                                                   ActionType action) {
    ASSERT_FALSE(authzSession->isExpired());
    ASSERT_EQ(authzSession->getAuthenticationMode(),
              AuthorizationSession::AuthenticationMode::kNone);
    ASSERT_FALSE(authzSession->isAuthenticated());
    ASSERT_EQ(authzSession->getAuthenticatedUser(), boost::none);
    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(resource, action));
}

void AuthorizationSessionTestFixture::assertExpired(const ResourcePattern& resource,
                                                    ActionType action) {
    ASSERT_TRUE(authzSession->isExpired());
    ASSERT_EQ(authzSession->getAuthenticationMode(),
              AuthorizationSession::AuthenticationMode::kNone);
    ASSERT_FALSE(authzSession->isAuthenticated());
    ASSERT_EQ(authzSession->getAuthenticatedUser(), boost::none);
    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(resource, action));
}

void AuthorizationSessionTestFixture::assertActive(const ResourcePattern& resource,
                                                   ActionType action) {
    ASSERT_FALSE(authzSession->isExpired());
    ASSERT_EQ(authzSession->getAuthenticationMode(),
              AuthorizationSession::AuthenticationMode::kConnection);
    ASSERT_TRUE(authzSession->isAuthenticated());
    ASSERT_NOT_EQUALS(authzSession->getAuthenticatedUser(), boost::none);
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(resource, action));
}

void AuthorizationSessionTestFixture::assertSecurityToken(const ResourcePattern& resource,
                                                          ActionType action) {
    ASSERT_FALSE(authzSession->isExpired());
    ASSERT_EQ(authzSession->getAuthenticationMode(),
              AuthorizationSession::AuthenticationMode::kSecurityToken);
    ASSERT_TRUE(authzSession->isAuthenticated());
    ASSERT_NOT_EQUALS(authzSession->getAuthenticatedUser(), boost::none);
    ASSERT_TRUE(authzSession->isAuthorizedForActionsOnResource(resource, action));
}

void AuthorizationSessionTestFixture::assertNotAuthorized(const ResourcePattern& resource,
                                                          ActionType action) {
    ASSERT_FALSE(authzSession->isAuthorizedForActionsOnResource(resource, action));
}

AggregateCommandRequest AuthorizationSessionTestFixture::buildAggReq(const NamespaceString& nss,
                                                                     const BSONArray& pipeline) {
    return uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        nss,
        BSON("aggregate" << nss.coll() << "pipeline" << pipeline << "cursor" << BSONObj() << "$db"
                         << nss.db_forTest()),
        boost::none,
        false /* apiStrict */,
        _sc));
}

AggregateCommandRequest AuthorizationSessionTestFixture::buildAggReq(const NamespaceString& nss,
                                                                     const BSONArray& pipeline,
                                                                     bool bypassDocValidation) {
    return uassertStatusOK(aggregation_request_helper::parseFromBSONForTests(
        nss,
        BSON("aggregate" << nss.coll() << "pipeline" << pipeline << "cursor" << BSONObj()
                         << "bypassDocumentValidation" << bypassDocValidation << "$db"
                         << nss.db_forTest()),
        boost::none,
        false /* apiStrict */,
        _sc));
}

}  // namespace mongo
