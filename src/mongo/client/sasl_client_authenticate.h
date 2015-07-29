/*    Copyright 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/authenticate.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"

namespace mongo {
class BSONObj;

/**
 * Attempts to authenticate "client" using the SASL protocol.
 *
 * Do not use directly in client code.  Use the auth::authenticateClient() method, instead.
 *
 * Test against NULL for availability.  Client driver must be compiled with SASL support _and_
 * client application must have successfully executed mongo::runGlobalInitializersOrDie() or its
 * ilk to make this functionality available.
 *
 * The "saslParameters" BSONObj should be initialized with zero or more of the
 * fields below.  Which fields are required depends on the mechanism.  Consult the
 * relevant IETF standards.
 *
 *     "mechanism": The std::string name of the sasl mechanism to use.  Mandatory.
 *     "autoAuthorize": Truthy values tell the server to automatically acquire privileges on
 *         all resources after successful authentication, which is the default.  Falsey values
 *         instruct the server to await separate privilege-acquisition commands.
 *     "user": The std::string name of the user to authenticate.
 *     "db": The database target of the auth command, which identifies the location
 *         of the credential information for the user.  May be "$external" if credential
 *         information is stored outside of the mongo cluster.
 *     "pwd": The password.
 *     "serviceName": The GSSAPI service name to use.  Defaults to "mongodb".
 *     "serviceHostname": The GSSAPI hostname to use.  Defaults to the name of the remote host.
 *
 * Other fields in saslParameters are silently ignored.
 *
 * Returns an OK status on success, and ErrorCodes::AuthenticationFailed if authentication is
 * rejected.  Other failures, all of which are tantamount to authentication failure, may also be
 * returned.
 */
extern void (*saslClientAuthenticate)(auth::RunCommandHook runCommand,
                                      StringData hostname,
                                      const BSONObj& saslParameters,
                                      auth::AuthCompletionHandler handler);

/**
 * Extracts the payload field from "cmdObj", and store it into "*payload".
 *
 * Sets "*type" to the BSONType of the payload field in cmdObj.
 *
 * If the type of the payload field is String, the contents base64 decodes and
 * stores into "*payload".  If the type is BinData, the contents are stored directly
 * into "*payload".  In all other cases, returns
 */
Status saslExtractPayload(const BSONObj& cmdObj, std::string* payload, BSONType* type);

// Constants

/// std::string name of the saslStart command.
extern const char* const saslStartCommandName;

/// std::string name of the saslContinue command.
extern const char* const saslContinueCommandName;

/// Name of the saslStart parameter indicating that the server should automatically grant the
/// connection all privileges associated with the user after successful authentication.
extern const char* const saslCommandAutoAuthorizeFieldName;

/// Name of the field contain the status code in responses from the server.
extern const char* const saslCommandCodeFieldName;

/// Name of the field containing the conversation identifier in server respones and saslContinue
/// commands.
extern const char* const saslCommandConversationIdFieldName;

/// Name of the field that indicates whether or not the server believes authentication has
/// completed successfully.
extern const char* const saslCommandDoneFieldName;

/// Field in which  to store error messages associated with non-success return codes.
extern const char* const saslCommandErrmsgFieldName;

/// Name of parameter to saslStart command indiciating the client's desired sasl mechanism.
extern const char* const saslCommandMechanismFieldName;

/// In the event that saslStart supplies an unsupported mechanism, the server responds with a
/// field by this name, with a list of supported mechanisms.
extern const char* const saslCommandMechanismListFieldName;

/// Field containing password information for saslClientAuthenticate().
extern const char* const saslCommandPasswordFieldName;

/// Field containing sasl payloads passed to and from the server.
extern const char* const saslCommandPayloadFieldName;

/// Field containing the std::string identifier of the user to authenticate in
/// saslClientAuthenticate().
extern const char* const saslCommandUserFieldName;

/// Field containing the std::string identifier of the database containing credential information,
/// or "$external" if the credential information is stored outside of the mongo cluster.
extern const char* const saslCommandUserDBFieldName;

/// Field overriding the FQDN of the hostname hosting the mongodb srevice in
/// saslClientAuthenticate().
extern const char* const saslCommandServiceHostnameFieldName;

/// Field overriding the name of the mongodb service saslClientAuthenticate().
extern const char* const saslCommandServiceNameFieldName;

/// Default database against which sasl authentication commands should run.
extern const char* const saslDefaultDBName;

/// Default sasl service name, "mongodb".
extern const char* const saslDefaultServiceName;

// Field whose value should be set to true if the field in saslCommandPasswordFieldName needs to
// be digested.
extern const char* const saslCommandDigestPasswordFieldName;
}
