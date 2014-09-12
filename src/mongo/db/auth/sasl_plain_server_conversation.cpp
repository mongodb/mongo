/*
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/auth/sasl_plain_server_conversation.h"

#include "mongo/db/auth/sasl_authentication_session.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/text.h"

namespace mongo {
   
    SaslPLAINServerConversation::SaslPLAINServerConversation(
                                                    SaslAuthenticationSession* saslAuthSession) :
        SaslServerConversation(saslAuthSession) {
    }

    SaslPLAINServerConversation::~SaslPLAINServerConversation() {};
    
    StatusWith<bool> SaslPLAINServerConversation::step(const StringData& inputData, 
                                                       std::string* outputData) {
        // Expecting user input on the form: user\0user\0pwd
        std::string input = inputData.toString();
        std::string pwd = "";
 
        try {
            _user = input.substr(0, inputData.find('\0'));
            pwd = input.substr(inputData.find('\0', _user.size()+1)+1);
        }
        catch (std::out_of_range& exception) {
            return StatusWith<bool>(ErrorCodes::AuthenticationFailed,
                mongoutils::str::stream() << "Incorrectly formatted PLAIN client message");
        }

        User* userObj;
        // The authentication database is also the source database for the user.
        Status status = _saslAuthSession->getAuthorizationSession()->getAuthorizationManager().
                acquireUser(_saslAuthSession->getOpCtxt(),
                            UserName(_user, _saslAuthSession->getAuthenticationDatabase()),
                            &userObj);

        if (!status.isOK()) {
            return StatusWith<bool>(status);
        }

        const User::CredentialData creds = userObj->getCredentials();
        _saslAuthSession->getAuthorizationSession()->getAuthorizationManager().
                releaseUser(userObj);

        std::string authDigest = createPasswordDigest(_user, pwd);
 
        if (authDigest != creds.password) {
            return StatusWith<bool>(ErrorCodes::AuthenticationFailed,
                mongoutils::str::stream() << "Incorrect user name or password");
        }

        *outputData = "";

        return StatusWith<bool>(true);
    }
    
}  // namespace mongo
