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

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/cstdint.h"  // Must be included at all because of SERVER-8086.

#include <gsasl.h>  // Must be included here because of SERVER-8086.

namespace mongo {

    /**
     * C++ wrapper around Gsasl_session.
     */
    class GsaslSession {
        MONGO_DISALLOW_COPYING(GsaslSession);
    public:
        GsaslSession();
        ~GsaslSession();

        /**
         * Initializes "this" as a client sasl session.
         *
         * May only be called once on an instance of GsaslSession, and may not be called on an
         * instance on which initializeServerSession has been called.
         *
         * "gsasl" is a pointer to a Gsasl library context that will exist for the rest of
         * the lifetime of "this".
         *
         * "mechanism" is a SASL mechanism name.
         *
         * "sessionHook" is user-supplied data associated with this session.  If is accessible in
         * the gsasl callback set on "gsasl" using gsasl_session_hook_get().  May be NULL.  Owned
         * by caller, and must stay in scope as long as this object.
         *
         * Returns Status::OK() on success, some other status on errors.
         */
        Status initializeClientSession(Gsasl* gsasl,
                                       const StringData& mechanism,
                                       void* sessionHook);

        /**
         * Initializes "this" as a server sasl session.
         *
         * May only be called once on an instance of GsaslSession, and may not be called on an
         * instance on which initializeClientSession has been called.
         *
         * "gsasl" is a pointer to a Gsasl library context that will exist for the rest of
         * the lifetime of "this".
         *
         * "mechanism" is a SASL mechanism name.
         *
         * "sessionHook" is user-supplied data associated with this session.  If is accessible in
         * the gsasl callback set on "gsasl" using gsasl_session_hook_get().  May be NULL.  Owned
         * by caller, and must stay in scope as long as this object.
         *
         * Returns Status::OK() on success, some other status on errors.
         */
        Status initializeServerSession(Gsasl* gsasl,
                                       const StringData& mechanism,
                                       void* sessionHook);

        /**
         * Returns the string name of the SASL mechanism in use in this session.
         *
         * Not valid before initializeServerSession() or initializeClientSession().
         */
        std::string getMechanism() const;

        /**
         * Sets a property on this session.
         *
         * Not valid before initializeServerSession() or initializeClientSession().
         */
        void setProperty(Gsasl_property property, const StringData& value);

        /**
         * Gets a property on this session.  Return an empty string if the property isn't set.
         *
         * Not valid before initializeServerSession() or initializeClientSession().
         */
        const std::string getProperty(Gsasl_property property) const;

        /**
         * Performs one more step on this session.
         *
         * Receives "inputData" from the other side and produces "*outputData" to send.
         *
         * Both "inputData" and "*outputData" are logically strings of bytes, not characters.
         *
         * For the first step by the authentication initiator, "inputData" should have 0 length.
         *
         * Returns Status::OK() on success.  In that case, isDone() can be queried to see if the
         * session expects another call to step().  If isDone() is true, the authentication has
         * completed successfully.
         *
         * Any return other than Status::OK() means that authentication has failed, but the specific
         * code or reason message may provide insight as to why.
         */
        Status step(const StringData& inputData, std::string* outputData);

        /**
         * Returns true if this session has completed successfully.
         *
         * That is, returns true if the session expects no more calls to step(), and all previous
         * calls to step() and initializeClientSession()/initializeServerSession() have returned
         * Status::OK().
         */
        bool isDone() const { return _done; }

    private:
        // Signature of gsas session start functions.
        typedef int (*GsaslSessionStartFn)(Gsasl*, const char*, Gsasl_session**);

        /**
         * Common helper code for initializing a session.
         *
         * Uses "sessionStartFn" to initialize the underlying Gsasl_session.
         */
        Status _initializeSession(GsaslSessionStartFn sessionStartFn,
                                  Gsasl* gsasl, const StringData& mechanism, void* sessionHook);

        /// Underlying C-library gsasl session object.
        Gsasl_session* _gsaslSession;

        /// See isDone(), above.
        bool _done;
    };

}  // namespace mongo
