/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/logical_session_id.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authorization_session_for_test.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class LogicalSessionIdTest : public ::mongo::unittest::Test {
public:
    AuthzManagerExternalStateMock* managerState;
    transport::TransportLayerMock transportLayer;
    transport::SessionHandle session;
    ServiceContextNoop serviceContext;
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext _opCtx;
    AuthzSessionExternalStateMock* sessionState;
    AuthorizationManager* authzManager;
    AuthorizationSessionForTest* authzSession;

    void setUp() {
        serverGlobalParams.featureCompatibility.version.store(
            ServerGlobalParams::FeatureCompatibility::Version::k36);
        session = transportLayer.createSession();
        client = serviceContext.makeClient("testClient", session);
        RestrictionEnvironment::set(
            session, stdx::make_unique<RestrictionEnvironment>(SockAddr(), SockAddr()));
        _opCtx = client->makeOperationContext();
        auto localManagerState = stdx::make_unique<AuthzManagerExternalStateMock>();
        managerState = localManagerState.get();
        managerState->setAuthzVersion(AuthorizationManager::schemaVersion26Final);
        auto uniqueAuthzManager =
            stdx::make_unique<AuthorizationManager>(std::move(localManagerState));
        authzManager = uniqueAuthzManager.get();
        AuthorizationManager::set(&serviceContext, std::move(uniqueAuthzManager));
        auto localSessionState = stdx::make_unique<AuthzSessionExternalStateMock>(authzManager);
        sessionState = localSessionState.get();

        auto localauthzSession =
            stdx::make_unique<AuthorizationSessionForTest>(std::move(localSessionState));
        authzSession = localauthzSession.get();

        AuthorizationSession::set(client.get(), std::move(localauthzSession));
        authzManager->setAuthEnabled(true);
    }

    User* addSimpleUser(UserName un) {
        ASSERT_OK(managerState->insertPrivilegeDocument(
            _opCtx.get(),
            BSON("user" << un.getUser() << "db" << un.getDB() << "credentials" << BSON("MONGODB-CR"
                                                                                       << "a")
                        << "roles"
                        << BSON_ARRAY(BSON("role"
                                           << "readWrite"
                                           << "db"
                                           << "test"))),
            BSONObj()));
        ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), un));
        return authzSession->lookupUser(un);
    }

    User* addClusterUser(UserName un) {
        ASSERT_OK(managerState->insertPrivilegeDocument(
            _opCtx.get(),
            BSON("user" << un.getUser() << "db" << un.getDB() << "credentials" << BSON("MONGODB-CR"
                                                                                       << "a")
                        << "roles"
                        << BSON_ARRAY(BSON("role"
                                           << "__system"
                                           << "db"
                                           << "admin"))),
            BSONObj()));
        ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), un));
        return authzSession->lookupUser(un);
    }
};

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithoutPassedUid) {
    auto id = UUID::gen();
    User* user = addSimpleUser(UserName("simple", "test"));

    LogicalSessionFromClient req;
    req.setId(id);

    LogicalSessionId lsid = makeLogicalSessionId(req, _opCtx.get());
    ASSERT_EQ(lsid.getId(), id);
    ASSERT_EQ(lsid.getUid(), user->getDigest());
}

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithoutPassedUidAndWithoutAuthedUser) {
    auto id = UUID::gen();

    LogicalSessionFromClient req;
    req.setId(id);

    ASSERT_THROWS(makeLogicalSessionId(req, _opCtx.get()), UserException);
}

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithPassedUidWithPermissions) {
    auto id = UUID::gen();
    auto uid = SHA256Block{};
    addClusterUser(UserName("cluster", "test"));

    LogicalSessionFromClient req;
    req.setId(id);
    req.setUid(uid);

    LogicalSessionId lsid = makeLogicalSessionId(req, _opCtx.get());

    ASSERT_EQ(lsid.getId(), id);
    ASSERT_EQ(lsid.getUid(), uid);
}

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithPassedUidWithoutAuthedUser) {
    auto id = UUID::gen();
    auto uid = SHA256Block{};

    LogicalSessionFromClient req;
    req.setId(id);
    req.setUid(uid);

    ASSERT_THROWS(makeLogicalSessionId(req, _opCtx.get()), UserException);
}

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithPassedUidWithoutPermissions) {
    auto id = UUID::gen();
    auto uid = SHA256Block{};
    addSimpleUser(UserName("simple", "test"));

    LogicalSessionFromClient req;
    req.setId(id);
    req.setUid(uid);

    ASSERT_THROWS(makeLogicalSessionId(req, _opCtx.get()), UserException);
}

TEST_F(LogicalSessionIdTest, GenWithUser) {
    User* user = addSimpleUser(UserName("simple", "test"));
    auto lsid = makeLogicalSessionId(_opCtx.get());

    ASSERT_EQ(lsid.getUid(), user->getDigest());
}

TEST_F(LogicalSessionIdTest, GenWithMultipleAuthedUsers) {
    addSimpleUser(UserName("simple", "test"));
    addSimpleUser(UserName("simple", "test2"));

    ASSERT_THROWS(makeLogicalSessionId(_opCtx.get()), UserException);
}

TEST_F(LogicalSessionIdTest, GenWithoutAuthedUser) {
    ASSERT_THROWS(makeLogicalSessionId(_opCtx.get()), UserException);
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_NoSessionIdNoTransactionNumber) {
    addSimpleUser(UserName("simple", "test"));
    initializeOperationSessionInfo(_opCtx.get(), BSON("TestCmd" << 1), true);

    ASSERT(!_opCtx->getLogicalSessionId());
    ASSERT(!_opCtx->getTxnNumber());
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_SessionIdNoTransactionNumber) {
    addSimpleUser(UserName("simple", "test"));
    LogicalSessionFromClient lsid{};
    lsid.setId(UUID::gen());

    initializeOperationSessionInfo(_opCtx.get(),
                                   BSON("TestCmd" << 1 << "lsid" << lsid.toBSON() << "OtherField"
                                                  << "TestField"),
                                   true);

    ASSERT(_opCtx->getLogicalSessionId());
    ASSERT_EQ(lsid.getId(), _opCtx->getLogicalSessionId()->getId());

    ASSERT(!_opCtx->getTxnNumber());
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_MissingSessionIdWithTransactionNumber) {
    addSimpleUser(UserName("simple", "test"));
    ASSERT_THROWS_CODE(
        initializeOperationSessionInfo(_opCtx.get(),
                                       BSON("TestCmd" << 1 << "txnNumber" << 100LL << "OtherField"
                                                      << "TestField"),
                                       true),
        UserException,
        ErrorCodes::IllegalOperation);
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_SessionIdAndTransactionNumber) {
    addSimpleUser(UserName("simple", "test"));
    LogicalSessionFromClient lsid;
    lsid.setId(UUID::gen());

    initializeOperationSessionInfo(
        _opCtx.get(),
        BSON("TestCmd" << 1 << "lsid" << lsid.toBSON() << "txnNumber" << 100LL << "OtherField"
                       << "TestField"),
        true);

    ASSERT(_opCtx->getLogicalSessionId());
    ASSERT_EQ(lsid.getId(), _opCtx->getLogicalSessionId()->getId());

    ASSERT(_opCtx->getTxnNumber());
    ASSERT_EQ(100, *_opCtx->getTxnNumber());
}

}  // namespace
}  // namespace mongo
