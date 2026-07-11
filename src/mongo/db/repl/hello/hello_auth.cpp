// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/hello/hello_auth.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/authenticate.h"
#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/auth/sasl_commands.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands/authentication_commands.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl

namespace mongo {

void handleHelloAuth(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     const HelloCommand& cmd,
                     const bool isInitialHandshake,
                     BSONObjBuilder* result) {
    // saslSupportedMechs: UserName -> List[String]
    if (auto userNameVariant = cmd.getSaslSupportedMechs()) {
        auto userName = UserName::parseFromVariant(userNameVariant.value());
        AuthenticationSession::doStep(
            opCtx, AuthenticationSession::StepType::kSaslSupportedMechanisms, [&](auto session) {
                session->setUserNameForSaslSupportedMechanisms(userName);

                auto& saslMechanismRegistry = SASLServerMechanismRegistry::get(opCtx->getService());
                saslMechanismRegistry.advertiseMechanismNamesForUser(opCtx, userName, result);
            });
    } else if (isInitialHandshake && gEnableDetailedConnectionHealthMetricLogLines.load()) {
        auto client = opCtx->getClient();
        auto metadata = ClientMetadata::get(client);
        LOGV2(10483900,
              "Connection not authenticating",
              "client"_attr = client->getRemote(),
              "doc"_attr = metadata ? metadata->getDocument() : BSONObj());
    }

    // speculativeAuthenticate: SaslStart -> SaslReply or Authenticate -> AuthenticateReply
    auto& specAuth = cmd.getSpeculativeAuthenticate();
    if (!specAuth) {
        return;
    }

    uassert(6656100,
            "Cannot specify speculativeAuthenticate with a tenantId",
            !dbName.tenantId() || dbName.tenantId() == TenantId::systemTenantId());

    uassert(ErrorCodes::BadValue,
            str::stream() << "hello." << auth::kSpeculativeAuthenticate
                          << " must be a non-empty Object",
            !specAuth->isEmpty());
    auto specCmd = specAuth->firstElementFieldNameStringData();

    if (specCmd == saslStartCommandName) {
        doSpeculativeSaslStart(opCtx, *specAuth, result);
    } else if (specCmd == auth::kAuthenticateCommand) {
        doSpeculativeAuthenticate(opCtx, *specAuth, result);
    } else {
        uasserted(51769,
                  str::stream() << "hello." << auth::kSpeculativeAuthenticate
                                << " unknown command: " << specCmd);
    }
}

}  // namespace mongo
