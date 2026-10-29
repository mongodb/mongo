// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/// String name of the saslStart command.
constexpr inline std::string_view saslStartCommandName{"saslStart"};

/// String name of the saslContinue command.
constexpr inline std::string_view saslContinueCommandName{"saslContinue"};

/// Name of the saslStart parameter indicating that the server should std::string_viewmatically
/// grant the connection all privileges associated with the user after successful authentication.
constexpr inline std::string_view saslCommandAutoAuthorizeFieldName{"std::string_viewAuthorize"};

/// Name of the field containing the conversation identifier in server respones and saslContinue
/// commands.
constexpr inline std::string_view saslCommandConversationIdFieldName{"conversationId"};

/// Name of the field that indicates whether or not the server believes authentication has
/// completed successfully.
constexpr inline std::string_view saslCommandDoneFieldName{"done"};

/// Name of parameter to saslStart command indiciating the client's desired sasl mechanism.
constexpr inline std::string_view saslCommandMechanismFieldName{"mechanism"};

/// In the event that saslStart supplies an unsupported mechanism, the server responds with a
/// field by this name, with a list of supported mechanisms.
constexpr inline std::string_view saslCommandMechanismListFieldName{"supportedMechanisms"};

/// Field containing password information for saslClientAuthenticate().
constexpr inline std::string_view saslCommandPasswordFieldName{"pwd"};

/// Field containing sasl payloads passed to and from the server.
constexpr inline std::string_view saslCommandPayloadFieldName{"payload"};

/// Field containing the string identifier of the user to authenticate in
/// saslClientAuthenticate().
constexpr inline std::string_view saslCommandUserFieldName{"user"};

/// Field containing the string identifier of the database containing credential information,
/// or "$external" if the credential information is stored outside of the mongo cluster.
constexpr inline std::string_view saslCommandUserDBFieldName{"db"};

/// Field overriding the FQDN of the hostname hosting the mongodb srevice in
/// saslClientAuthenticate().
constexpr inline std::string_view saslCommandServiceHostnameFieldName{"serviceHostname"};

/// Field overriding the name of the mongodb service saslClientAuthenticate().
constexpr inline std::string_view saslCommandServiceNameFieldName{"serviceName"};

/// Default database against which sasl authentication commands should run.
constexpr inline std::string_view saslDefaultDBName{"$external"};

/// Default sasl service name, "mongodb".
constexpr inline std::string_view saslDefaultServiceName{"mongodb"};

// Field whose value should be set to true if the field in saslCommandPasswordFieldName needs to
// be digested.
constexpr inline std::string_view saslCommandDigestPasswordFieldName{"digestPassword"};

// Field containing optional session token information for MONGODB-AWS sasl mechanism.
constexpr inline std::string_view saslCommandIamSessionToken{"awsIamSessionToken"};

// Field containing optional access token to be passed in directly for the MONGODB-OIDC SASL
// mechanism.
constexpr inline std::string_view saslCommandOIDCAccessToken{"oidcAccessToken"};

// Field in saslStart.options for mechanisms which omit empty "OK" exchange.
constexpr inline std::string_view saslCommandOptionSkipEmptyExchange{"skipEmptyExchange"};

}  // namespace mongo
