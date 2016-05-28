/*
 *    Copyright (C) 2012 10gen, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include "mongo/db/auth/sasl_authentication_session.h"

#include <boost/range/size.hpp>

#include "mongo/base/init.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/commands.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
SaslAuthenticationSession::SaslAuthenticationSessionFactoryFn SaslAuthenticationSession::create;

// Mechanism name constants.
const char SaslAuthenticationSession::mechanismCRAMMD5[] = "CRAM-MD5";
const char SaslAuthenticationSession::mechanismDIGESTMD5[] = "DIGEST-MD5";
const char SaslAuthenticationSession::mechanismSCRAMSHA1[] = "SCRAM-SHA-1";
const char SaslAuthenticationSession::mechanismGSSAPI[] = "GSSAPI";
const char SaslAuthenticationSession::mechanismPLAIN[] = "PLAIN";

/**
 * Standard method in mongodb for determining if "authenticatedUser" may act as "requestedUser."
 *
 * The standard rule in MongoDB is simple.  The authenticated user name must be the same as the
 * requested user name.
 */
bool isAuthorizedCommon(SaslAuthenticationSession* session,
                        StringData requestedUser,
                        StringData authenticatedUser) {
    return requestedUser == authenticatedUser;
}

SaslAuthenticationSession::SaslAuthenticationSession(AuthorizationSession* authzSession)
    : AuthenticationSession(AuthenticationSession::SESSION_TYPE_SASL),
      _authzSession(authzSession),
      _saslStep(0),
      _conversationId(0),
      _autoAuthorize(false),
      _done(false) {}

SaslAuthenticationSession::~SaslAuthenticationSession(){};

StringData SaslAuthenticationSession::getAuthenticationDatabase() const {
    if (Command::testCommandsEnabled && _authenticationDatabase == "admin" &&
        getPrincipalId() == internalSecurity.user->getName().getUser()) {
        // Allows authenticating as the internal user against the admin database.  This is to
        // support the auth passthrough test framework on mongos (since you can't use the local
        // database on a mongos, so you can't auth as the internal user without this).
        return internalSecurity.user->getName().getDB();
    } else {
        return _authenticationDatabase;
    }
}

}  // namespace mongo
