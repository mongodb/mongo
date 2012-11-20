/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "mongo/db/auth/authentication_session.h"

namespace mongo {

    typedef unsigned long long nonce64;

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
