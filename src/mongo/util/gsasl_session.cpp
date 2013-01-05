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

#include "mongo/util/gsasl_session.h"

#include <cstdlib>

#include "mongo/util/assert_util.h"

namespace mongo {

    GsaslSession::GsaslSession() : _gsaslSession(NULL), _done(false) {}

    GsaslSession::~GsaslSession() {
        if (_gsaslSession)
            gsasl_finish(_gsaslSession);
    }

    std::string GsaslSession::getMechanism() const {
        return gsasl_mechanism_name(_gsaslSession);
    }

    void GsaslSession::setProperty(Gsasl_property property, const StringData& value) {
        gsasl_property_set_raw(_gsaslSession, property, value.rawData(), value.size());
    }

    const std::string GsaslSession::getProperty(Gsasl_property property) const {
        const char* prop = gsasl_property_fast(_gsaslSession, property);
        if (prop == NULL) {
            return "";
        }
        return prop;
    }

    Status GsaslSession::initializeClientSession(Gsasl* gsasl,
                                                 const StringData& mechanism,
                                                 void* sessionHook) {
        return _initializeSession(&gsasl_client_start, gsasl, mechanism, sessionHook);
    }

    Status GsaslSession::initializeServerSession(Gsasl* gsasl,
                                                 const StringData& mechanism,
                                                 void* sessionHook) {
        return _initializeSession(&gsasl_server_start, gsasl, mechanism, sessionHook);
    }

    Status GsaslSession::_initializeSession(
            GsaslSessionStartFn sessionStartFn,
            Gsasl* gsasl, const StringData& mechanism, void* sessionHook) {

        if (_done || _gsaslSession)
            return Status(ErrorCodes::CannotReuseObject, "Cannot reuse GsaslSession.");

        int rc = sessionStartFn(gsasl, mechanism.toString().c_str(), &_gsaslSession);
        switch (rc) {
        case GSASL_OK:
            gsasl_session_hook_set(_gsaslSession, sessionHook);
            return Status::OK();
        case GSASL_UNKNOWN_MECHANISM:
            return Status(ErrorCodes::BadValue, gsasl_strerror(rc));
        default:
            return Status(ErrorCodes::ProtocolError, gsasl_strerror(rc));
        }
    }

    Status GsaslSession::step(const StringData& inputData, std::string* outputData) {
        char* output;
        size_t outputSize;
        int rc = gsasl_step(_gsaslSession,
                            inputData.rawData(), inputData.size(),
                            &output, &outputSize);

        if (GSASL_OK == rc)
            _done = true;

        switch (rc) {
        case GSASL_OK:
        case GSASL_NEEDS_MORE:
            *outputData = std::string(output, output + outputSize);
            free(output);
            return Status::OK();
        case GSASL_AUTHENTICATION_ERROR:
            return Status(ErrorCodes::AuthenticationFailed, gsasl_strerror(rc));
        default:
            return Status(ErrorCodes::ProtocolError, gsasl_strerror(rc));
        }
    }

}  // namespace mongo
