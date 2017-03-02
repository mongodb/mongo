/*
 *    Copyright (C) 2010 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define BONGO_LOG_DEFAULT_COMPONENT ::bongo::logger::LogComponent::kAccessControl

#include "bongo/platform/basic.h"

#include "bongo/db/auth/security_key.h"

#include <string>
#include <sys/stat.h>
#include <vector>

#include "bongo/base/status_with.h"
#include "bongo/client/sasl_client_authenticate.h"
#include "bongo/crypto/mechanism_scram.h"
#include "bongo/db/auth/action_set.h"
#include "bongo/db/auth/action_type.h"
#include "bongo/db/auth/authorization_manager.h"
#include "bongo/db/auth/internal_user_auth.h"
#include "bongo/db/auth/privilege.h"
#include "bongo/db/auth/sasl_options.h"
#include "bongo/db/auth/security_file.h"
#include "bongo/db/auth/user.h"
#include "bongo/db/server_options.h"
#include "bongo/util/log.h"
#include "bongo/util/password_digest.h"

namespace bongo {

using std::string;

bool setUpSecurityKey(const string& filename) {
    StatusWith<std::string> keyString = bongo::readSecurityFile(filename);
    if (!keyString.isOK()) {
        log() << keyString.getStatus().reason();
        return false;
    }

    std::string str = std::move(keyString.getValue());
    const unsigned long long keyLength = str.size();
    if (keyLength < 6 || keyLength > 1024) {
        log() << " security key in " << filename << " has length " << keyLength
              << ", must be between 6 and 1024 chars";
        return false;
    }

    // Generate BONGODB-CR and SCRAM credentials for the internal user based on
    // the keyfile.
    User::CredentialData credentials;
    credentials.password =
        bongo::createPasswordDigest(internalSecurity.user->getName().getUser().toString(), str);

    BSONObj creds = scram::generateCredentials(credentials.password,
                                               saslGlobalParams.scramIterationCount.load());
    credentials.scram.iterationCount = creds[scram::iterationCountFieldName].Int();
    credentials.scram.salt = creds[scram::saltFieldName].String();
    credentials.scram.storedKey = creds[scram::storedKeyFieldName].String();
    credentials.scram.serverKey = creds[scram::serverKeyFieldName].String();

    internalSecurity.user->setCredentials(credentials);

    int clusterAuthMode = serverGlobalParams.clusterAuthMode.load();
    if (clusterAuthMode == ServerGlobalParams::ClusterAuthMode_keyFile ||
        clusterAuthMode == ServerGlobalParams::ClusterAuthMode_sendKeyFile) {
        setInternalUserAuthParams(
            BSON(saslCommandMechanismFieldName << "SCRAM-SHA-1" << saslCommandUserDBFieldName
                                               << internalSecurity.user->getName().getDB()
                                               << saslCommandUserFieldName
                                               << internalSecurity.user->getName().getUser()
                                               << saslCommandPasswordFieldName
                                               << credentials.password
                                               << saslCommandDigestPasswordFieldName
                                               << false));
    }

    return true;
}

}  // namespace bongo
