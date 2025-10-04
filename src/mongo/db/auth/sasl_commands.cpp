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


#include "mongo/db/auth/sasl_commands.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/authenticate.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/sasl_payload.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <iosfwd>
#include <memory>
#include <ratio>
#include <set>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace auth {
namespace {

using std::stringstream;

class CmdSaslStart : public SaslStartCmdVersion1Gen<CmdSaslStart> {
public:
    std::set<StringData> sensitiveFieldNames() const final {
        return {SaslStartCommand::kPayloadFieldName};
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx) override;
    };

    std::string help() const final {
        return "First step in a SASL authentication conversation.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuth() const override {
        return false;
    }

    HandshakeRole handshakeRole() const final {
        return HandshakeRole::kAuth;
    }
};
MONGO_REGISTER_COMMAND(CmdSaslStart).forRouter().forShard();

class CmdSaslContinue : public SaslContinueCmdVersion1Gen<CmdSaslContinue> {
public:
    std::set<StringData> sensitiveFieldNames() const final {
        return {SaslContinueCommand::kPayloadFieldName};
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx) override;
    };

    std::string help() const final {
        return "Subsequent steps in a SASL authentication conversation.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool requiresAuth() const final {
        return false;
    }

    HandshakeRole handshakeRole() const final {
        return HandshakeRole::kAuth;
    }
};
MONGO_REGISTER_COMMAND(CmdSaslContinue).forRouter().forShard();

SaslReply doSaslStep(OperationContext* opCtx,
                     const SaslPayload& payload,
                     AuthenticationSession* session) {
    auto mechanismPtr = session->getMechanism();
    invariant(mechanismPtr);
    auto& mechanism = *mechanismPtr;

    // Passing in a payload and extracting a responsePayload
    StatusWith<std::string> swResponse = [&] {
        ScopedCallbackTimer st([&](Duration<std::micro> elapsed) {
            BSONObjBuilder bob;

            auto currentStepOpt = mechanism.currentStep();
            if (currentStepOpt != boost::none) {
                bob << "step" << static_cast<int>(currentStepOpt.get());
            }

            auto totalStepOpt = mechanism.totalSteps();
            if (totalStepOpt != boost::none) {
                bob << "step_total" << static_cast<int>(totalStepOpt.get());
            }

            bob << "duration_micros" << elapsed.count();

            session->metrics()->appendMetric(bob.obj());
        });

        return mechanism.step(opCtx, payload.get());
    }();

    if (!swResponse.isOK()) {
        sleepmillis(saslGlobalParams.authFailedDelay.load());
        uassertStatusOK(swResponse);
    }

    if (mechanism.isSuccess()) {
        auto request = uassertStatusOK(mechanism.makeUserRequest(opCtx));
        auto expirationTime = mechanism.getExpirationTime();
        uassertStatusOK(AuthorizationSession::get(opCtx->getClient())
                            ->addAndAuthorizeUser(opCtx, std::move(request), expirationTime));

        session->markSuccessful();
    }

    SaslReply reply;
    reply.setConversationId(1);
    reply.setDone(mechanism.isSuccess());

    SaslPayload replyPayload(swResponse.getValue());
    replyPayload.serializeAsBase64(payload.getSerializeAsBase64());
    reply.setPayload(std::move(replyPayload));

    return reply;
}

void warnIfCompressed(OperationContext* opCtx) {
    if (opCtx->isOpCompressed()) {
        LOGV2_WARNING(6697500,
                      "SASL commands should not be run over the OP_COMPRESSED message type. This "
                      "invocation may have security implications.");
    }
}

SaslReply runSaslContinue(OperationContext* opCtx,
                          AuthenticationSession* session,
                          const SaslContinueCommand& request);

SaslReply CmdSaslStart::Invocation::typedRun(OperationContext* opCtx) try {
    return AuthenticationSession::doStep(
        opCtx, AuthenticationSession::StepType::kSaslStart, [&](auto session) {
            return runSaslStart(opCtx, session, request());
        });
} catch (const DBException& ex) {
    switch (ex.code()) {
        case ErrorCodes::MechanismUnavailable:
        case ErrorCodes::ProtocolError:
            throw;
        default:
            uasserted(AuthorizationManager::authenticationFailedStatus.code(),
                      AuthorizationManager::authenticationFailedStatus.reason());
    }
}


SaslReply CmdSaslContinue::Invocation::typedRun(OperationContext* opCtx) try {
    return AuthenticationSession::doStep(
        opCtx, AuthenticationSession::StepType::kSaslContinue, [&](auto session) {
            return runSaslContinue(opCtx, session, request());
        });
} catch (const DBException& ex) {
    if (ex.code() == ErrorCodes::ProtocolError) {
        throw;
    }

    uasserted(AuthorizationManager::authenticationFailedStatus.code(),
              AuthorizationManager::authenticationFailedStatus.reason());
}

SaslReply runSaslContinue(OperationContext* opCtx,
                          AuthenticationSession* session,
                          const SaslContinueCommand& cmd) {
    warnIfCompressed(opCtx);
    opCtx->markKillOnClientDisconnect();

    uassert(ErrorCodes::ProtocolError,
            "sasl: Mismatched conversation id",
            cmd.getConversationId() == 1);

    return doSaslStep(opCtx, cmd.getPayload(), session);
}

constexpr auto kDBFieldName = "db"_sd;
}  // namespace

SaslReply runSaslStart(OperationContext* opCtx,
                       AuthenticationSession* session,
                       const SaslStartCommand& request) {
    session->metrics()->restart();

    warnIfCompressed(opCtx);
    opCtx->markKillOnClientDisconnect();

    // Note that while updateDatabase can throw, it should not be able to for saslStart.
    session->updateDatabase(
        DatabaseNameUtil::serialize(request.getDbName(), request.getSerializationContext()),
        request.getMechanism() == auth::kMechanismMongoX509);
    session->setMechanismName(request.getMechanism());

    auto mechanism = uassertStatusOK(
        SASLServerMechanismRegistry::get(opCtx->getService())
            .getServerMechanism(request.getMechanism(),
                                DatabaseNameUtil::serialize(request.getDbName(),
                                                            request.getSerializationContext())));

    uassert(ErrorCodes::BadValue,
            "Plaintext mechanisms may not be used with speculativeSaslStart",
            !session->isSpeculative() ||
                mechanism->properties().hasAllProperties(
                    SecurityPropertySet({SecurityProperty::kNoPlainText})));

    session->setMechanism(std::move(mechanism), request.getOptions());

    return auth::doSaslStep(opCtx, request.getPayload(), session);
}

}  // namespace auth

void doSpeculativeSaslStart(OperationContext* opCtx,
                            const BSONObj& sourceObj,
                            BSONObjBuilder* result) try {
    auth::warnIfCompressed(opCtx);
    // TypedCommands expect DB overrides in the "$db" field,
    // but saslStart coming from the Hello command has it in the "db" field.
    // Rewrite it for handling here.
    BSONObjBuilder bob;
    bool hasDBField = false;
    for (const auto& elem : sourceObj) {
        if (elem.fieldName() == auth::kDBFieldName) {
            bob.appendAs(elem, auth::SaslStartCommand::kDbNameFieldName);
            hasDBField = true;
        } else {
            bob.append(elem);
        }
    }
    if (!hasDBField) {
        return;
    }

    const auto cmdObj = bob.obj();

    AuthenticationSession::doStep(
        opCtx, AuthenticationSession::StepType::kSpeculativeSaslStart, [&](auto session) {
            auto request =
                auth::SaslStartCommand::parse(cmdObj, IDLParserContext("speculative saslStart"));
            auto reply = auth::runSaslStart(opCtx, session, request);
            result->append(auth::kSpeculativeAuthenticate, reply.toBSON());
        });
} catch (...) {
    // Treat failure like we never even got a speculative start.
}

}  // namespace mongo
