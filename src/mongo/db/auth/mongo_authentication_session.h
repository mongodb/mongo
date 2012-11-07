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

#include "mongo/db/auth/authentication_session.h"
#include "mongo/db/nonce.h"

namespace mongo {

    /**
     * Authentication session data for a nonce-challenge-response authentication of the
     * type used in the Mongo nonce-authenticate protocol.
     *
     * The only session data is the nonce sent to the client.
     */
    class MongoAuthenticationSession : public AuthenticationSession {
        MONGO_DISALLOW_COPYING(MongoAuthenticationSession);
    public:
        explicit MongoAuthenticationSession(nonce64 nonce);
        virtual ~MongoAuthenticationSession();

        nonce64 getNonce() const { return _nonce; }

    private:
        const nonce64 _nonce;
    };

}  // namespace mongo
