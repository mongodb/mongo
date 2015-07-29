/*    Copyright 2015 MongoDB Inc.
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

#include <string>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/md5.h"

namespace mongo {

class DBClientWithCommands;
class BSONObj;

namespace auth {

using AuthResponse = StatusWith<executor::RemoteCommandResponse>;
using AuthCompletionHandler = stdx::function<void(AuthResponse)>;
using RunCommandResultHandler = AuthCompletionHandler;
using RunCommandHook =
    stdx::function<void(executor::RemoteCommandRequest, RunCommandResultHandler)>;

/**
 * Names for supported authentication mechanisms.
 */

extern const char* const kMechanismMongoCR;
extern const char* const kMechanismMongoX509;
extern const char* const kMechanismSaslPlain;
extern const char* const kMechanismGSSAPI;
extern const char* const kMechanismScramSha1;

/**
 * Authenticate a user.
 *
 * Pass the default hostname for this client in through "hostname." If SSL is enabled and
 * there is a stored client subject name, pass that through the "clientSubjectName" parameter.
 * Otherwise, "clientSubjectName" will be silently ignored, pass in any string.
 *
 * The "params" BSONObj should be initialized with some of the fields below.  Which fields
 * are required depends on the mechanism, which is mandatory.
 *
 *     "mechanism": The std::string name of the sasl mechanism to use.  Mandatory.
 *     "user": The std::string name of the user to authenticate.  Mandatory.
 *     "db": The database target of the auth command, which identifies the location
 *         of the credential information for the user.  May be "$external" if
 *         credential information is stored outside of the mongo cluster.  Mandatory.
 *     "pwd": The password data.
 *     "digestPassword": Boolean, set to true if the "pwd" is undigested (default).
 *     "serviceName": The GSSAPI service name to use.  Defaults to "mongodb".
 *     "serviceHostname": The GSSAPI hostname to use.  Defaults to the name of the remote
 *          host.
 *
 * Other fields in "params" are silently ignored. A "params" object can be constructed
 * using the buildAuthParams() method.
 *
 * If a "handler" is provided, this call will execute asynchronously and "handler" will be
 * invoked when authentication has completed.  If no handler is provided, authenticateClient
 * will run synchronously.
 *
 * Returns normally on success, and throws on error.  Throws a DBException with getCode() ==
 * ErrorCodes::AuthenticationFailed if authentication is rejected.  All other exceptions are
 * tantamount to authentication failure, but may also indicate more serious problems.
 */
void authenticateClient(const BSONObj& params,
                        StringData hostname,
                        StringData clientSubjectName,
                        RunCommandHook runCommand,
                        AuthCompletionHandler handler = AuthCompletionHandler());

/**
 * Build a BSONObject representing parameters to be passed to authenticateClient(). Takes
 * the following fields:
 *
 *     @dbname: The database target of the auth command.
 *     @username: The std::string name of the user to authenticate.
 *     @passwordText: The std::string representing the user's password.
 *     @digestPassword: Set to true if the password is undigested.
 */
BSONObj buildAuthParams(StringData dbname,
                        StringData username,
                        StringData passwordText,
                        bool digestPassword);

/**
 * Return the field name for the database containing credential information.
 */
StringData getSaslCommandUserDBFieldName();

/**
 * Return the field name for the user to authenticate.
 */
StringData getSaslCommandUserFieldName();

}  // namespace auth
}  // namespace mongo
