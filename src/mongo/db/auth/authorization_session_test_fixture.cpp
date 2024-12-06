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

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/namespace_string.h"
namespace mongo {

void AuthorizationSessionTestFixture::setUp() {
    // AuthorizationManager must be initialized prior to creating Client objects.
    auto localManagerState = std::make_unique<FailureCapableAuthzManagerExternalStateMock>();
    managerState = localManagerState.get();
    auto uniqueAuthzManager = std::make_unique<AuthorizationManagerImpl>(
        getServiceContext(), std::move(localManagerState));

    _session = transportLayer.createSession();
    _client = getServiceContext()->makeClient("testClient", _session);
    RestrictionEnvironment::set(_session,
                                std::make_unique<RestrictionEnvironment>(SockAddr(), SockAddr()));
    _opCtx = _client->makeOperationContext();
    managerState->setAuthzVersion(AuthorizationManager::schemaVersion26Final);
    authzManager = uniqueAuthzManager.get();
    AuthorizationManager::set(getServiceContext(), std::move(uniqueAuthzManager));
    auto localSessionState = std::make_unique<AuthzSessionExternalStateMock>(authzManager);
    sessionState = localSessionState.get();
    authzSession = std::make_unique<AuthorizationSessionForTest>(
        std::move(localSessionState), AuthorizationSessionImpl::InstallMockForTestingOrAuthImpl{});
    authzManager->setAuthEnabled(true);

    credentials =
        BSON("SCRAM-SHA-1" << scram::Secrets<SHA1Block>::generateCredentials(
                                  "a", saslGlobalParams.scramSHA1IterationCount.load())
                           << "SCRAM-SHA-256"
                           << scram::Secrets<SHA256Block>::generateCredentials(
                                  "a", saslGlobalParams.scramSHA256IterationCount.load()));
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

    return managerState->insertPrivilegeDocument(_opCtx.get(), userDoc.obj(), {});
}
}  // namespace mongo
