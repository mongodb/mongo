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

#include "mongo/db/audit.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
constexpr auto kDiagnosticLogLevel = 0;
}

AuthenticationSession::StepGuard::StepGuard(OperationContext* opCtx, StepType currentStep)
    : _opCtx(opCtx), _currentStep(currentStep) {
    auto client = _opCtx->getClient();

    LOGV2_DEBUG(
        5286300, kDiagnosticLogLevel, "Starting authentication step", "step"_attr = _currentStep);

    auto& maybeSession = _get(client);
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

    switch (_currentStep) {
        case StepType::kSaslSupportedMechanisms: {
            createSession();
            authCounter.incSaslSupportedMechanismsReceived();
        } break;
        case StepType::kSpeculativeAuthenticate:
        case StepType::kSpeculativeSaslStart: {
            createSession();
            maybeSession->_isSpeculative = true;
        } break;
        case StepType::kAuthenticate:
        case StepType::kSaslStart: {
            createSession();
        } break;
        case StepType::kSaslContinue: {
            uassert(ErrorCodes::ProtocolError, "No SASL session state found", maybeSession);
        } break;
    }
}

AuthenticationSession::StepGuard::~StepGuard() {
    auto& maybeSession = _get(_opCtx->getClient());
    if (maybeSession) {
        LOGV2_DEBUG(5286301,
                    kDiagnosticLogLevel,
                    "Finished authentication step",
                    "step"_attr = _currentStep);
        if (maybeSession->isFinished()) {
            // We're done with this session, reset it.
            maybeSession.reset();
        }
    }
}

AuthenticationSession* AuthenticationSession::get(Client* client) {
    auto& maybeSession = _get(client);
    tassert(5286302, "Unable to retrieve authentication session", static_cast<bool>(maybeSession));
    return &(*maybeSession);
}

void AuthenticationSession::setMechanism(std::unique_ptr<ServerMechanismBase> mech,
                                         boost::optional<BSONObj> options) {
    tassert(5286303, "Attempted to override previous authentication mechanism", !_mech);

    _mech = std::move(mech);
    if (options) {
        uassertStatusOK(_mech->setOptions(options->getOwned()));
    }

    if (_mech && _mech->isClusterMember()) {
        setAsClusterMember();
    }

    LOGV2_DEBUG(5286304, kDiagnosticLogLevel, "Determined mechanism for authentication");
}

void AuthenticationSession::setAsClusterMember() {
    _isClusterMember = true;

    LOGV2_DEBUG(5286305, kDiagnosticLogLevel, "Marking as cluster member");
}

void AuthenticationSession::markSuccessful() {
    // Log success.
    _isFinished = true;
    LOGV2_DEBUG(5286306,
                kDiagnosticLogLevel,
                "Successfully authenticated",
                "isSpeculative"_attr = isSpeculative(),
                "isClusterMember"_attr = isClusterMember());
}

void AuthenticationSession::markFailed(const Status& status) {
    // Log the error.
    _isFinished = true;
    LOGV2_DEBUG(5286307,
                kDiagnosticLogLevel,
                "Failed to authenticate",
                "isSpeculative"_attr = isSpeculative(),
                "isClusterMember"_attr = isClusterMember(),
                "error"_attr = status);
}

}  // namespace mongo
