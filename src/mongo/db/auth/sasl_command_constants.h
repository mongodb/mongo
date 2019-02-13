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

#include "mongo/base/string_data.h"

namespace mongo {

/// String name of the saslStart command.
constexpr auto saslStartCommandName = "saslStart"_sd;

/// String name of the saslContinue command.
constexpr auto saslContinueCommandName = "saslContinue"_sd;

/// Name of the saslStart parameter indicating that the server should automatically grant the
/// connection all privileges associated with the user after successful authentication.
constexpr auto saslCommandAutoAuthorizeFieldName = "autoAuthorize"_sd;

/// Name of the field containing the conversation identifier in server respones and saslContinue
/// commands.
constexpr auto saslCommandConversationIdFieldName = "conversationId"_sd;

/// Name of the field that indicates whether or not the server believes authentication has
/// completed successfully.
constexpr auto saslCommandDoneFieldName = "done"_sd;

/// Name of parameter to saslStart command indiciating the client's desired sasl mechanism.
constexpr auto saslCommandMechanismFieldName = "mechanism"_sd;

/// In the event that saslStart supplies an unsupported mechanism, the server responds with a
/// field by this name, with a list of supported mechanisms.
constexpr auto saslCommandMechanismListFieldName = "supportedMechanisms"_sd;

/// Field containing password information for saslClientAuthenticate().
constexpr auto saslCommandPasswordFieldName = "pwd"_sd;

/// Field containing sasl payloads passed to and from the server.
constexpr auto saslCommandPayloadFieldName = "payload"_sd;

/// Field containing the string identifier of the user to authenticate in
/// saslClientAuthenticate().
constexpr auto saslCommandUserFieldName = "user"_sd;

/// Field containing the string identifier of the database containing credential information,
/// or "$external" if the credential information is stored outside of the mongo cluster.
constexpr auto saslCommandUserDBFieldName = "db"_sd;

/// Field overriding the FQDN of the hostname hosting the mongodb srevice in
/// saslClientAuthenticate().
constexpr auto saslCommandServiceHostnameFieldName = "serviceHostname"_sd;

/// Field overriding the name of the mongodb service saslClientAuthenticate().
constexpr auto saslCommandServiceNameFieldName = "serviceName"_sd;

/// Default database against which sasl authentication commands should run.
constexpr auto saslDefaultDBName = "$external"_sd;

/// Default sasl service name, "mongodb".
constexpr auto saslDefaultServiceName = "mongodb"_sd;

// Field whose value should be set to true if the field in saslCommandPasswordFieldName needs to
// be digested.
constexpr auto saslCommandDigestPasswordFieldName = "digestPassword"_sd;

}  // namespace mongo
