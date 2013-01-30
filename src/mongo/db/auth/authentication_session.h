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
