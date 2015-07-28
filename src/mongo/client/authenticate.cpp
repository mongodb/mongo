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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/platform/basic.h"

#include "mongo/client/authenticate.h"

#include "mongo/bson/json.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/config.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/password_digest.h"

namespace mongo {
namespace auth {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

namespace {

// TODO: These constants need to be cleaned up.
const char* const saslCommandUserSourceFieldName = "userSource";
const BSONObj getnoncecmdobj = fromjson("{getnonce:1}");

bool isOk(const BSONObj& o) {
    return getStatusFromCommandResult(o).isOK();
}

BSONObj getFallbackAuthParams(const BSONObj& params) {
    if (params["fallbackParams"].type() != Object) {
        return BSONObj();
    }
    return params["fallbackParams"].Obj();
}

// Use the MONGODB-CR protocol to authenticate as "username" against the database "dbname",
// with the given password.  If digestPassword is false, the password is assumed to be
// pre-digested.  Returns false on failure, and sets "errmsg".
bool authMongoCR(RunCommandHook runCommand,
                 StringData dbname,
                 StringData username,
                 StringData password_text,
                 BSONObj* info,
                 bool digestPassword) {
    auto request = RemoteCommandRequest();
    request.cmdObj = getnoncecmdobj;
    request.dbname = dbname.toString();

    std::string password = password_text.toString();
    if (digestPassword)
        password = createPasswordDigest(username, password_text);

    std::string nonce;
    invariant(info != nullptr && runCommand);
    auto runCommandHandler = [&info](StatusWith<RemoteCommandResponse> response) {
        *info = response.getValue().data.getOwned();
    };

    runCommand(request, runCommandHandler);
    if (!isOk(*info)) {
        return false;
    }

    {
        BSONElement e = info->getField("nonce");
        verify(e.type() == String);
        nonce = e.valuestr();
    }

    BSONObjBuilder b;
    {
        b << "authenticate" << 1 << "nonce" << nonce << "user" << username;
        md5digest d;
        {
            md5_state_t st;
            md5_init(&st);
            md5_append(&st, (const md5_byte_t*)nonce.c_str(), nonce.size());
            md5_append(&st, (const md5_byte_t*)username.rawData(), username.size());
            md5_append(&st, (const md5_byte_t*)password.c_str(), password.size());
            md5_finish(&st, d);
        }
        b << "key" << digestToString(d);
        request.cmdObj = b.done();
    }

    runCommand(request, runCommandHandler);
    return isOk(*info);
}

// Use the MONGODB-X509 protocol to authenticate as "username." The certificate details
// has already been communicated automatically as part of the connect call.
// Returns false on failure and set "errmsg".
bool authX509(RunCommandHook runCommand, StringData dbname, StringData username, BSONObj* info) {
    BSONObjBuilder cmdBuilder;
    cmdBuilder << "authenticate" << 1 << "mechanism"
               << "MONGODB-X509"
               << "user" << username;

    auto request = RemoteCommandRequest();
    request.dbname = dbname.toString();
    request.cmdObj = cmdBuilder.done();

    runCommand(request,
               [&info](StatusWith<RemoteCommandResponse> response) {
                   *info = response.getValue().data.getOwned();
               });

    return isOk(*info);
}

void auth(RunCommandHook runCommand,
          const BSONObj& params,
          StringData hostname,
          StringData clientName) {
    std::string mechanism;

    uassertStatusOK(bsonExtractStringField(params, saslCommandMechanismFieldName, &mechanism));

    uassert(17232,
            "You cannot specify both 'db' and 'userSource'. Please use only 'db'.",
            !(params.hasField(saslCommandUserDBFieldName) &&
              params.hasField(saslCommandUserSourceFieldName)));

    if (mechanism == StringData("MONGODB-CR", StringData::LiteralTag())) {
        std::string db;
        if (params.hasField(saslCommandUserSourceFieldName)) {
            uassertStatusOK(bsonExtractStringField(params, saslCommandUserSourceFieldName, &db));
        } else {
            uassertStatusOK(bsonExtractStringField(params, saslCommandUserDBFieldName, &db));
        }
        std::string user;
        uassertStatusOK(bsonExtractStringField(params, saslCommandUserFieldName, &user));
        std::string password;
        uassertStatusOK(bsonExtractStringField(params, saslCommandPasswordFieldName, &password));
        bool digestPassword;
        uassertStatusOK(bsonExtractBooleanFieldWithDefault(
            params, saslCommandDigestPasswordFieldName, true, &digestPassword));
        BSONObj result;
        uassert(result["code"].Int(),
                result.toString(),
                authMongoCR(runCommand, db, user, password, &result, digestPassword));
    }
#ifdef MONGO_CONFIG_SSL
    else if (mechanism == StringData("MONGODB-X509", StringData::LiteralTag())) {
        std::string db;
        if (params.hasField(saslCommandUserSourceFieldName)) {
            uassertStatusOK(bsonExtractStringField(params, saslCommandUserSourceFieldName, &db));
        } else {
            uassertStatusOK(bsonExtractStringField(params, saslCommandUserDBFieldName, &db));
        }
        std::string user;
        uassertStatusOK(bsonExtractStringField(params, saslCommandUserFieldName, &user));

        uassert(ErrorCodes::AuthenticationFailed,
                "Please enable SSL on the client-side to use the MONGODB-X509 "
                "authentication mechanism.",
                clientName.toString() != "");

        uassert(ErrorCodes::AuthenticationFailed,
                "Username \"" + user + "\" does not match the provided client certificate user \"" +
                    clientName.toString() + "\"",
                user == clientName.toString());

        BSONObj result;
        uassert(result["code"].Int(), result.toString(), authX509(runCommand, db, user, &result));
    }
#endif
    else if (saslClientAuthenticate != nullptr) {
        uassertStatusOK(saslClientAuthenticate(runCommand, hostname, params));
    } else {
        uasserted(ErrorCodes::BadValue,
                  mechanism + " mechanism support not compiled into client library.");
    }
};

}  // namespace

void authenticateClient(const BSONObj& params,
                        StringData hostname,
                        StringData clientName,
                        RunCommandHook runCommand) {
    try {
        auth(runCommand, params, hostname, clientName);
        return;
    } catch (const UserException& ex) {
        if (getFallbackAuthParams(params).isEmpty() ||
            (ex.getCode() != ErrorCodes::BadValue && ex.getCode() != ErrorCodes::CommandNotFound)) {
            throw ex;
        }
    }

    // BadValue or CommandNotFound indicates unsupported auth mechanism so fall back to
    // MONGODB-CR for 2.6 compatibility.
    auth(runCommand, getFallbackAuthParams(params), hostname, clientName);
}

BSONObj buildAuthParams(StringData dbname,
                        StringData username,
                        StringData passwordText,
                        bool digestPassword) {
    return BSON(saslCommandMechanismFieldName
                << "SCRAM-SHA-1" << saslCommandUserDBFieldName << dbname << saslCommandUserFieldName
                << username << saslCommandPasswordFieldName << passwordText
                << saslCommandDigestPasswordFieldName << digestPassword);
}

StringData getSaslCommandUserDBFieldName() {
    return saslCommandUserDBFieldName;
}

StringData getSaslCommandUserFieldName() {
    return saslCommandUserFieldName;
}

}  // namespace auth
}  // namespace mongo
