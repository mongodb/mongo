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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/auth/role_name.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/initialize_operation_session_info.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/service_liaison_mock.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_impl.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/db/session/sessions_collection_mock.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

class LogicalSessionIdTest : public ServiceContextTest {
public:
    AuthzManagerExternalStateMock* managerState;
    transport::TransportLayerMock transportLayer;
    std::shared_ptr<transport::Session> session = transportLayer.createSession();
    ServiceContext::UniqueOperationContext _opCtx;
    AuthorizationSession* authzSession;

    LogicalSessionIdTest() {
        auto session = std::make_shared<transport::MockSession>(
            HostAndPort(), SockAddr(), SockAddr(), nullptr);
        auto localManagerState = std::make_unique<AuthzManagerExternalStateMock>();
        managerState = localManagerState.get();
        {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            managerState->setAuthzVersion(opCtx, AuthorizationManager::schemaVersion26Final);
        }
        auto authzManager =
            std::make_unique<AuthorizationManagerImpl>(getService(), std::move(localManagerState));
        authzManager->setAuthEnabled(true);
        AuthorizationManager::set(getService(), std::move(authzManager));
        Client::releaseCurrent();
        Client::initThread(getThreadName(), getServiceContext()->getService(), session);
        authzSession = AuthorizationSession::get(getClient());
        _opCtx = makeOperationContext();

        auto localServiceLiaison =
            std::make_unique<MockServiceLiaison>(std::make_shared<MockServiceLiaisonImpl>());
        auto localSessionsCollection = std::make_unique<MockSessionsCollection>(
            std::make_shared<MockSessionsCollectionImpl>());

        auto localLogicalSessionCache = std::make_unique<LogicalSessionCacheImpl>(
            std::move(localServiceLiaison),
            std::move(localSessionsCollection),
            [](OperationContext*, SessionsCollection&, Date_t) {
                return 0; /* No op*/
            });

        LogicalSessionCache::set(getServiceContext(), std::move(localLogicalSessionCache));
    }

    User* addSimpleUser(UserName un) {
        const auto creds = BSON("SCRAM-SHA-1" << scram::Secrets<SHA1Block>::generateCredentials(
                                    "a", saslGlobalParams.scramSHA1IterationCount.load()));
        ASSERT_OK(managerState->insertPrivilegeDocument(
            _opCtx.get(),
            BSON("user" << un.getUser() << "db" << un.getDB() << "credentials" << creds << "roles"
                        << BSON_ARRAY(BSON("role"
                                           << "readWrite"
                                           << "db"
                                           << "test"))),
            BSONObj()));
        ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), {un, boost::none}, boost::none));
        return authzSession->lookupUser(un);
    }

    User* addClusterUser(UserName un) {
        const auto creds = BSON("SCRAM-SHA-256" << scram::Secrets<SHA256Block>::generateCredentials(
                                    "a", saslGlobalParams.scramSHA256IterationCount.load()));
        ASSERT_OK(managerState->insertPrivilegeDocument(
            _opCtx.get(),
            BSON("user" << un.getUser() << "db" << un.getDB() << "credentials" << creds << "roles"
                        << BSON_ARRAY(BSON("role"
                                           << "__system"
                                           << "db"
                                           << "admin"))),
            BSONObj()));
        ASSERT_OK(authzSession->addAndAuthorizeUser(_opCtx.get(), {un, boost::none}, boost::none));
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

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithTxnUUID) {
    auto id = UUID::gen();
    auto txnUUID = UUID::gen();
    User* user = addSimpleUser(UserName("simple", "test"));

    LogicalSessionFromClient req;
    req.setId(id);
    req.setTxnUUID(txnUUID);

    LogicalSessionId lsid = makeLogicalSessionId(req, _opCtx.get());
    ASSERT_EQ(lsid.getId(), id);
    ASSERT_EQ(lsid.getUid(), user->getDigest());
    ASSERT_EQ(*lsid.getTxnUUID(), txnUUID);
}

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithTxnNumberWithoutTxnUUID) {
    auto id = UUID::gen();
    TxnNumber txnNumber(35);
    addSimpleUser(UserName("simple", "test"));

    LogicalSessionFromClient req;
    req.setId(id);
    req.setTxnNumber(txnNumber);

    ASSERT_THROWS_CODE(
        makeLogicalSessionId(req, _opCtx.get()), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithTxnNumberAndTxnUUID) {
    auto id = UUID::gen();
    TxnNumber txnNumber(35);
    auto txnUUID = UUID::gen();
    User* user = addSimpleUser(UserName("simple", "test"));

    LogicalSessionFromClient req;
    req.setId(id);
    req.setTxnNumber(txnNumber);
    req.setTxnUUID(txnUUID);

    LogicalSessionId lsid = makeLogicalSessionId(req, _opCtx.get());
    ASSERT_EQ(lsid.getId(), id);
    ASSERT_EQ(lsid.getUid(), user->getDigest());
    ASSERT_EQ(*lsid.getTxnNumber(), txnNumber);
    ASSERT_EQ(*lsid.getTxnUUID(), txnUUID);

    // Convert back to LogicalSessionFromClient through OperationSessionInfoFromClient and verify it
    // matches the original LogicalSessionFromClient and LogicalSessionId.
    OperationSessionInfoFromClient osiFromClient{lsid, boost::none};
    auto& req2 = *osiFromClient.getSessionId();

    ASSERT_EQ(req2.getId(), req.getId());
    ASSERT_EQ(*req2.getTxnNumber(), *req.getTxnNumber());
    ASSERT_EQ(*req2.getTxnUUID(), *req.getTxnUUID());

    ASSERT_EQ(req2.getId(), lsid.getId());
    ASSERT_EQ(req2.getUid(), lsid.getUid());
    ASSERT_EQ(*req2.getTxnNumber(), *lsid.getTxnNumber());
    ASSERT_EQ(*req2.getTxnUUID(), *lsid.getTxnUUID());
}

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithoutPassedUidAndWithoutAuthedUser) {
    auto id = UUID::gen();

    LogicalSessionFromClient req;
    req.setId(id);

    ASSERT_THROWS(makeLogicalSessionId(req, _opCtx.get()), AssertionException);
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

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithOwnUidWithNonImpersonatePermissions) {
    User* user = addSimpleUser(UserName("simple", "test"));
    auto id = UUID::gen();
    auto uid = user->getDigest();

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

    ASSERT_THROWS(makeLogicalSessionId(req, _opCtx.get()), AssertionException);
}

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithPassedNonMatchingUidWithoutPermissions) {
    auto id = UUID::gen();
    auto uid = SHA256Block{};
    addSimpleUser(UserName("simple", "test"));

    LogicalSessionFromClient req;
    req.setId(id);
    req.setUid(uid);

    ASSERT_THROWS(makeLogicalSessionId(req, _opCtx.get()), AssertionException);
}

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithPassedMatchingUidWithoutPermissions) {
    auto id = UUID::gen();
    User* user = addSimpleUser(UserName("simple", "test"));
    auto uid = user->getDigest();

    LogicalSessionFromClient req;
    req.setId(id);
    req.setUid(uid);

    LogicalSessionId lsid = makeLogicalSessionId(req, _opCtx.get());

    ASSERT_EQ(lsid.getId(), id);
    ASSERT_EQ(lsid.getUid(), uid);
}

TEST_F(LogicalSessionIdTest, GenWithUser) {
    User* user = addSimpleUser(UserName("simple", "test"));
    auto lsid = makeLogicalSessionId(_opCtx.get());

    ASSERT_EQ(lsid.getUid(), user->getDigest());
}

TEST_F(LogicalSessionIdTest, GenWithoutAuthedUser) {
    ASSERT_THROWS(makeLogicalSessionId(_opCtx.get()), AssertionException);
}

OperationSessionInfoFromClient initializeOpSessionInfoWithRequestBody(
    OperationContext* opCtx,
    const BSONObj& requestBody,
    bool requiresAuth,
    bool attachToOpCtx,
    bool isReplSetMemberOrMongos) {
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired,
        DatabaseName::createDatabaseName_forTest(boost::none, "test_unused_dbname"),

        requestBody,
        BSONObj());
    auto osi = OperationSessionInfoFromClient::parse(IDLParserContext{"OperationSessionInfo"},
                                                     opMsgRequest.body);
    return initializeOperationSessionInfo(opCtx,
                                          opMsgRequest.getValidatedTenantId(),
                                          osi,
                                          requiresAuth,
                                          attachToOpCtx,
                                          isReplSetMemberOrMongos);
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_NoSessionIdNoTransactionNumber) {
    addSimpleUser(UserName("simple", "test"));
    initializeOpSessionInfoWithRequestBody(_opCtx.get(),
                                           BSON("TestCmd" << 1),
                                           true /* requiresAuth */,
                                           true /* attachToOpCtx */,
                                           true /* isReplSetMemberOrMongos */);

    ASSERT(!_opCtx->getLogicalSessionId());
    ASSERT(!_opCtx->getTxnNumber());
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_SessionIdNoTransactionNumber) {
    addSimpleUser(UserName("simple", "test"));
    LogicalSessionFromClient lsid{};
    lsid.setId(UUID::gen());

    initializeOpSessionInfoWithRequestBody(_opCtx.get(),
                                           BSON("TestCmd" << 1 << "lsid" << lsid.toBSON()
                                                          << "OtherField"
                                                          << "TestField"),
                                           true /* requiresAuth */,
                                           true /* attachToOpCtx */,
                                           true /* isReplSetMemberOrMongos */);

    ASSERT(_opCtx->getLogicalSessionId());
    ASSERT_EQ(lsid.getId(), _opCtx->getLogicalSessionId()->getId());

    ASSERT(!_opCtx->getTxnNumber());
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_MissingSessionIdWithTransactionNumber) {
    addSimpleUser(UserName("simple", "test"));
    ASSERT_THROWS_CODE(initializeOpSessionInfoWithRequestBody(
                           _opCtx.get(),
                           BSON("TestCmd" << 1 << "txnNumber" << 100LL << "OtherField"
                                          << "TestField"),
                           true /* requiresAuth */,
                           true /* attachToOpCtx */,
                           true /* isReplSetMemberOrMongos */),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_SessionIdAndTransactionNumber) {
    addSimpleUser(UserName("simple", "test"));
    LogicalSessionFromClient lsid;
    lsid.setId(UUID::gen());

    initializeOpSessionInfoWithRequestBody(_opCtx.get(),
                                           BSON("TestCmd" << 1 << "lsid" << lsid.toBSON()
                                                          << "txnNumber" << 100LL << "OtherField"
                                                          << "TestField"),
                                           true /* requiresAuth */,
                                           true /* attachToOpCtx */,
                                           true /* isReplSetMemberOrMongos */);

    ASSERT(_opCtx->getLogicalSessionId());
    ASSERT_EQ(lsid.getId(), _opCtx->getLogicalSessionId()->getId());

    ASSERT(_opCtx->getTxnNumber());
    ASSERT_EQ(100, *_opCtx->getTxnNumber());
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_IsReplSetMemberOrMongosFalse) {
    addSimpleUser(UserName("simple", "test"));
    LogicalSessionFromClient lsid;
    lsid.setId(UUID::gen());

    ASSERT_THROWS_CODE(
        initializeOpSessionInfoWithRequestBody(
            _opCtx.get(),
            BSON("TestCmd" << 1 << "lsid" << lsid.toBSON() << "txnNumber" << 100LL << "OtherField"
                           << "TestField"),
            true /* requiresAuth */,
            true /* attachToOpCtx */,
            false /* isReplSetMemberOrMongos */),
        AssertionException,
        ErrorCodes::IllegalOperation);
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_IgnoresInfoIfNoCache) {
    addSimpleUser(UserName("simple", "test"));
    LogicalSessionFromClient lsid;
    lsid.setId(UUID::gen());

    LogicalSessionCache::set(_opCtx->getServiceContext(), nullptr);

    auto sessionInfo = initializeOpSessionInfoWithRequestBody(
        _opCtx.get(),
        BSON("TestCmd" << 1 << "lsid" << lsid.toBSON() << "txnNumber" << 100LL << "OtherField"
                       << "TestField"),
        true /* requiresAuth */,
        true /* attachToOpCtx */,
        true /* isReplSetMemberOrMongos */);
    ASSERT(sessionInfo.getSessionId() == boost::none);
    ASSERT(sessionInfo.getTxnNumber() == boost::none);
    ASSERT(sessionInfo.getStartTransaction() == boost::none);
    ASSERT(sessionInfo.getAutocommit() == boost::none);
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_IgnoresInfoIfDoNotAttachToOpCtx) {
    addSimpleUser(UserName("simple", "test"));
    LogicalSessionFromClient lsid;
    lsid.setId(UUID::gen());

    auto sessionInfo = initializeOpSessionInfoWithRequestBody(
        _opCtx.get(),
        BSON("TestCmd" << 1 << "lsid" << lsid.toBSON() << "txnNumber" << 100LL << "OtherField"
                       << "TestField"),
        true /* requiresAuth */,
        false /* attachToOpCtx */,
        true /* isReplSetMemberOrMongos */);

    ASSERT(sessionInfo.getSessionId() == boost::none);
    ASSERT(sessionInfo.getTxnNumber() == boost::none);
    ASSERT(sessionInfo.getStartTransaction() == boost::none);
    ASSERT(sessionInfo.getAutocommit() == boost::none);

    ASSERT(_opCtx->getLogicalSessionId() == boost::none);
    ASSERT(_opCtx->getTxnNumber() == boost::none);
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_VerifyUIDEvenIfDoNotAttachToOpCtx) {
    addSimpleUser(UserName("simple", "test"));
    LogicalSessionFromClient lsid;
    lsid.setId(UUID::gen());

    auto invalidDigest = SHA256Block::computeHash({ConstDataRange("hacker")});
    lsid.setUid(invalidDigest);

    ASSERT_THROWS_CODE(initializeOpSessionInfoWithRequestBody(
                           _opCtx.get(),
                           BSON("TestCmd" << 1 << "lsid" << lsid.toBSON() << "txnNumber" << 100LL),
                           true /* requiresAuth */,
                           false /* attachToOpCtx */,
                           true /* isReplSetMemberOrMongos */),
                       AssertionException,
                       ErrorCodes::Unauthorized);
}

TEST_F(LogicalSessionIdTest, InitializeOperationSessionInfo_SendingInfoFailsInDirectClient) {
    const std::vector<BSONObj> operationSessionParameters{
        {BSON("lsid" << makeLogicalSessionIdForTest().toBSON())},
        {BSON("txnNumber" << 1LL)},
        {BSON("autocommit" << true)},
        {BSON("startTransaction" << true)}};


    _opCtx->getClient()->setInDirectClient(true);

    for (const auto& param : operationSessionParameters) {
        BSONObjBuilder commandBuilder = BSON("count"
                                             << "foo");
        commandBuilder.appendElements(param);

        ASSERT_THROWS_CODE(
            initializeOpSessionInfoWithRequestBody(_opCtx.get(),
                                                   commandBuilder.obj(),
                                                   true /* requiresAuth */,
                                                   true /* attachToOpCtx */,
                                                   true /* isReplSetMemberOrMongos */),
            AssertionException,
            50891);
    }

    _opCtx->getClient()->setInDirectClient(false);
}

TEST_F(LogicalSessionIdTest, ConstructorFromClientWithTooLongName) {
    auto id = UUID::gen();

    addSimpleUser(UserName(std::string(kMaximumUserNameLengthForLogicalSessions + 1, 'x'), "test"));

    LogicalSessionFromClient req;
    req.setId(id);

    ASSERT_THROWS(makeLogicalSessionId(req, _opCtx.get()), AssertionException);
}

}  // namespace
}  // namespace mongo
