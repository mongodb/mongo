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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

#include "mongo/db/auth/authentication_session.h"

#include "mongo/client/authenticate.h"
#include "mongo/db/audit.h"
#include "mongo/db/client.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
constexpr auto kDiagnosticLogLevel = 3;

Status crossVerifyUserNames(const UserName& oldUser, const UserName& newUser) noexcept {
    if (oldUser.getFullName().empty()) {
        return Status::OK();
    }

    if (!getTestCommandsEnabled()) {
        // Authenticating the __system@local user to the admin database on mongos is required
        // by the auth passthrough test suite, hence we forgive this set of errors in testing.

        if (oldUser.getDB() != newUser.getDB()) {
            return {ErrorCodes::ProtocolError,
                    "Attempt to switch database target during SASL authentication."};
        }
    }

    if (oldUser.getUser().empty() || newUser.getUser().empty()) {
        // If we don't have a user and our databases match, no harm and nothing more to do.
        return Status::OK();
    }

    if (oldUser.getUser() != newUser.getUser()) {
        return {ErrorCodes::ProtocolError, "Attempt to switch user during SASL authentication."};
    }

    return Status::OK();
}

const auto getAuthenticationSession =
    Client::declareDecoration<boost::optional<AuthenticationSession>>();

class AuthenticationClientObserver final : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client* client) override {}

    void onDestroyClient(Client* client) override {
        auto& maybeSession = getAuthenticationSession(client);
        if (maybeSession) {
            maybeSession->markFailed(
                {ErrorCodes::AuthenticationAbandoned,
                 "Authentication session abandoned, client has likely disconnected"});
        }
    }

    void onCreateOperationContext(OperationContext* opCtx) override {}
    void onDestroyOperationContext(OperationContext* opCtx) override {}
};

auto registerer = ServiceContext::ConstructorActionRegisterer{
    "AuthenticationClientObserver", [](ServiceContext* service) {
        service->registerClientObserver(std::make_unique<AuthenticationClientObserver>());
    }};
}  // namespace

AuthenticationSession::StepGuard::StepGuard(OperationContext* opCtx, StepType currentStep)
    : _opCtx(opCtx), _currentStep(currentStep) {
    auto client = _opCtx->getClient();

    LOGV2_DEBUG(
        5286300, kDiagnosticLogLevel, "Starting authentication step", "step"_attr = _currentStep);

    auto& maybeSession = getAuthenticationSession(client);
    ON_BLOCK_EXIT([&] {
        if (maybeSession) {
            // If we successfully made/kept a session, update it and track it.
            auto& session = *maybeSession;
            _session = &session;
        }
    });

    auto createSession = [&] {
        if (maybeSession) {
            maybeSession->markFailed(
                {ErrorCodes::AuthenticationAbandoned, "Overridden by new authentication session"});
        }
        maybeSession.emplace(client);
    };

    auto startActiveSession = [&] {
        if (maybeSession) {
            invariant(maybeSession->_lastStep);
            auto lastStep = *maybeSession->_lastStep;
            if (lastStep == StepType::kSaslSupportedMechanisms) {
                // We can follow saslSupportedMechanisms with saslStart or authenticate.
                return;
            }
        }

        createSession();
    };

    switch (_currentStep) {
        case StepType::kSaslSupportedMechanisms: {
            createSession();
            authCounter.incSaslSupportedMechanismsReceived();
        } break;
        case StepType::kSpeculativeAuthenticate:
        case StepType::kSpeculativeSaslStart: {
            startActiveSession();
            maybeSession->_isSpeculative = true;
        } break;
        case StepType::kAuthenticate:
        case StepType::kSaslStart: {
            startActiveSession();
        } break;
        case StepType::kSaslContinue: {
            uassert(ErrorCodes::ProtocolError, "No SASL session state found", maybeSession);

            uassert(ErrorCodes::ProtocolError,
                    "saslContinue must follow saslStart",
                    maybeSession->_mech);
        } break;
    }
}

AuthenticationSession::StepGuard::~StepGuard() {
    auto& maybeSession = getAuthenticationSession(_opCtx->getClient());
    if (maybeSession) {
        LOGV2_DEBUG(5286301,
                    kDiagnosticLogLevel,
                    "Finished authentication step",
                    "step"_attr = _currentStep);
        if (maybeSession->_isFinished) {
            // We're done with this session, reset it.
            maybeSession.reset();
        } else {
            maybeSession->_lastStep = _currentStep;
        }
    }
}

AuthenticationSession* AuthenticationSession::get(Client* client) {
    auto& maybeSession = getAuthenticationSession(client);
    tassert(5286302, "Unable to retrieve authentication session", static_cast<bool>(maybeSession));
    return &(*maybeSession);
}

void AuthenticationSession::setMechanismName(StringData mechanismName) {
    LOGV2_DEBUG(
        5286200, kDiagnosticLogLevel, "Setting mechanism name", "mechanism"_attr = mechanismName);
    tassert(5286201, "Attempt to change the mechanism name", _mechName.empty());

    _mechName = mechanismName.toString();
    _mechCounter = authCounter.getMechanismCounter(_mechName);
    _mechCounter->incAuthenticateReceived();
    if (_isSpeculative) {
        _mechCounter->incSpeculativeAuthenticateReceived();
    }
}

void AuthenticationSession::_verifyUserNameFromSaslSupportedMechanisms(const UserName& userName) {
    if (auto status = crossVerifyUserNames(_ssmUserName, userName); !status.isOK()) {
        LOGV2(5286202,
              "Different user name was supplied to saslSupportedMechs",
              "error"_attr = status);

        // Reset _ssmUserName since we have found a conflict.
        auto ssmUserName = std::exchange(_ssmUserName, {});
        audit::logAuthentication(_client,
                                 auth::kSaslSupportedMechanisms,
                                 std::move(ssmUserName),
                                 ErrorCodes::AuthenticationAbandoned);
    }
}

void AuthenticationSession::setUserNameForSaslSupportedMechanisms(UserName userName) {
    _verifyUserNameFromSaslSupportedMechanisms(userName);

    _ssmUserName = userName;
}

void AuthenticationSession::updateUserName(UserName userName) {
    LOGV2_DEBUG(5286203, kDiagnosticLogLevel, "Updating user name", "userName"_attr = userName);

    _verifyUserNameFromSaslSupportedMechanisms(userName);
    uassertStatusOK(crossVerifyUserNames(_userName, userName));
    _userName = userName;
}

void AuthenticationSession::setMechanism(std::unique_ptr<ServerMechanismBase> mech,
                                         boost::optional<BSONObj> options) {
    tassert(5286303, "Attempted to override previous authentication mechanism", !_mech);

    _mech = std::move(mech);
    if (options) {
        uassertStatusOK(_mech->setOptions(options->getOwned()));
    }

    LOGV2_DEBUG(5286304, kDiagnosticLogLevel, "Determined mechanism for authentication");
}

void AuthenticationSession::setAsClusterMember() {
    if (std::exchange(_isClusterMember, true)) {
        return;
    }

    _mechCounter->incClusterAuthenticateReceived();

    LOGV2_DEBUG(5286305, kDiagnosticLogLevel, "Marking as cluster member");
}

void AuthenticationSession::_finish() {
    _isFinished = true;
    if (_mech) {
        // Since both isClusterMember() and getPrincipalName() can return differently over the
        // course of authentication, only get the values when we finish.
        if (_mech->isClusterMember()) {
            setAsClusterMember();
        }
        updateUserName({_mech->getPrincipalName(), _mech->getAuthenticationDatabase()});
    }
}

void AuthenticationSession::markSuccessful() {
    _finish();

    _mechCounter->incAuthenticateSuccessful();
    if (_isClusterMember) {
        _mechCounter->incClusterAuthenticateSuccessful();
    }
    if (_isSpeculative) {
        _mechCounter->incSpeculativeAuthenticateSuccessful();
    }

    audit::logAuthentication(_client, _mechName, _userName, ErrorCodes::OK);

    LOGV2_DEBUG(5286306,
                kDiagnosticLogLevel,
                "Successfully authenticated",
                "client"_attr = _client->getRemote(),
                "isSpeculative"_attr = _isSpeculative,
                "isClusterMember"_attr = _isClusterMember,
                "mechanism"_attr = _mechName,
                "user"_attr = _userName.getUser(),
                "db"_attr = _userName.getDB());
}

void AuthenticationSession::markFailed(const Status& status) {
    _finish();

    audit::logAuthentication(_client, _mechName, _userName, status.code());

    LOGV2_DEBUG(5286307,
                kDiagnosticLogLevel,
                "Failed to authenticate",
                "client"_attr = _client->getRemote(),
                "isSpeculative"_attr = _isSpeculative,
                "isClusterMember"_attr = _isClusterMember,
                "mechanism"_attr = _mechName,
                "user"_attr = _userName.getUser(),
                "db"_attr = _userName.getDB(),
                "error"_attr = status);
}

}  // namespace mongo
