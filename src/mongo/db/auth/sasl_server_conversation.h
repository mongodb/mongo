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

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/auth/sasl_authentication_session.h"
#include "mongo/db/auth/user.h"

namespace mongo {

class SaslAuthenticationSession;
template <typename T>
class StatusWith;

/**
 * Abstract class for implementing the server-side
 * of a SASL mechanism conversation.
 */
class SaslServerConversation {
    MONGO_DISALLOW_COPYING(SaslServerConversation);

public:
    /**
     * Implements the server side of a SASL authentication mechanism.
     *
     * "saslAuthSession" is the corresponding SASLAuthenticationSession.
     * "saslAuthSession" must stay in scope until the SaslServerConversation's
     *  destructor completes.
     *
     **/
    explicit SaslServerConversation(SaslAuthenticationSession* saslAuthSession)
        : _saslAuthSession(saslAuthSession), _user("") {}

    virtual ~SaslServerConversation();

    /**
     * Performs one step of the server side of the authentication session,
     * consuming "inputData" and producing "*outputData".
     *
     * A return of Status::OK() indicates successful progress towards authentication.
     * A return of !Status::OK() indicates failed authentication
     *
     * A return of true means that the authentication process has finished.
     * A return of false means that the authentication process has more steps.
     *
     */
    virtual StatusWith<bool> step(StringData inputData, std::string* outputData) = 0;

    /**
     * Gets the SASL principal id (user name) for the conversation
     **/
    std::string getPrincipalId();

protected:
    SaslAuthenticationSession* _saslAuthSession;
    std::string _user;
};

}  // namespace mongo
