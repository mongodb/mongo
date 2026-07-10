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
