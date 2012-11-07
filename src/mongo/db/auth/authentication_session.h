/*
 *    Copyright (C) 2012 10gen Inc.
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
 */

#pragma once

#include "mongo/base/disallow_copying.h"

namespace mongo {

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
            SESSION_TYPE_SASL  // SASL authentication mechanism.
        };

        virtual ~AuthenticationSession() {}

        /**
         * Return an identifer of the type of session, so that a caller can safely cast it and
         * extract the type-specific data stored within.
         */
        SessionType getType() const { return _sessionType; }

    protected:
        explicit AuthenticationSession(SessionType sessionType) : _sessionType(sessionType) {}

    private:
        const SessionType _sessionType;
    };

}  // namespace mongo
