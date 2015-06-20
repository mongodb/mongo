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

#include <memory>

#include "mongo/base/disallow_copying.h"

namespace mongo {

class ClientBasic;

/**
 * Abstract type representing an ongoing authentication session.
 *
 * An example subclass is MongoAuthenticationSession.
 */
class AuthenticationSession {
    MONGO_DISALLOW_COPYING(AuthenticationSession);

public:
    enum SessionType {
        SESSION_TYPE_MONGO,  // The mongo-specific challenge-response authentication mechanism.
        SESSION_TYPE_SASL    // SASL authentication mechanism.
    };

    /**
     * Sets the authentication session for the given "client" to "newSession".
     */
    static void set(ClientBasic* client, std::unique_ptr<AuthenticationSession> newSession);

    /**
     * Swaps "client"'s current authentication session with "other".
     */
    static void swap(ClientBasic* client, std::unique_ptr<AuthenticationSession>& other);

    virtual ~AuthenticationSession() = default;

    /**
     * Return an identifer of the type of session, so that a caller can safely cast it and
     * extract the type-specific data stored within.
     */
    SessionType getType() const {
        return _sessionType;
    }

protected:
    explicit AuthenticationSession(SessionType sessionType) : _sessionType(sessionType) {}

private:
    const SessionType _sessionType;
};

}  // namespace mongo
