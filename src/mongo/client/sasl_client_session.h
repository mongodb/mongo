// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/authentication_metrics.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace mongo {

/**
 * Base class for the client side of a SASL authentication conversation.
 *
 * To use, create an instance, then use setParameter() to configure the authentication
 * parameters.  Once all parameters are set, call initialize() to initialize the client state
 * machine.  Finally, use repeated calls to step() to generate messages to send to the server
 * and process server responses.
 *
 * The required parameters vary by mechanism, but all mechanisms require parameterServiceName,
 * parameterServiceHostname, parameterMechanism and parameterUser.  All of the required
 * parameters must be UTF-8 encoded strings with no embedded NUL characters.  The
 * parameterPassword parameter is not constrained.
 */
class [[MONGO_MOD_UNFORTUNATELY_OPEN]] SaslClientSession {
    SaslClientSession(const SaslClientSession&) = delete;
    SaslClientSession& operator=(const SaslClientSession&) = delete;

public:
    typedef std::function<SaslClientSession*(const std::string&)> SaslClientSessionFactoryFn;
    static SaslClientSessionFactoryFn create;

    /**
     * Identifiers of parameters used to configure a SaslClientSession.
     */
    enum Parameter {
        parameterServiceName = 0,
        parameterServiceHostname,
        parameterServiceHostAndPort,
        parameterMechanism,
        parameterUser,
        parameterPassword,
        parameterAWSSessionToken,
        parameterOIDCAccessToken,
        numParameters  // Must be last
    };

    SaslClientSession();
    virtual ~SaslClientSession();

    /**
     * Sets the parameter identified by "id" to "value".
     *
     * The value of "id" must be one of the legal values of Parameter less than numParameters.
     * May be called repeatedly for the same value of "id", with the last "value" replacing
     * previous values.
     *
     * The session object makes and owns a copy of the data in "value".
     */
    virtual void setParameter(Parameter id, std::string_view value);

    /**
     * Returns true if "id" identifies a parameter previously set by a call to setParameter().
     */
    virtual bool hasParameter(Parameter id);

    /**
     * Returns the value of a previously set parameter.
     *
     * If parameter "id" was never set, returns an empty std::string_view.  Note that a parameter
     * may be explicitly set to std::string_view(), so use hasParameter() to distinguish those
     * cases.
     *
     * The session object owns the storage behind the returned std::string_view, which will remain
     * valid until setParameter() is called with the same value of "id", or the session object
     * goes out of scope.
     */
    virtual std::string_view getParameter(Parameter id);

    /**
     * Initializes a session for use.
     *
     * Call exactly once, after setting any parameters you intend to set via setParameter().
     */
    virtual Status initialize() = 0;

    /**
     * Takes one step of the SASL protocol on behalf of the client.
     *
     * Caller should provide data from the server side of the conversation in "inputData", or an
     * empty std::string_view() if none is available.  If the client should make a response to the
     * server, stores the response into "*outputData".
     *
     * Returns Status::OK() on success.  Any other return value indicates a failed
     * authentication, though the specific return value may provide insight into the cause of
     * the failure (e.g., ProtocolError, AuthenticationFailed).
     *
     * In the event that this method does not return Status::OK(), authentication has failed.
     * When step() returns Status::OK() and isSuccess() returns true,
     * authentication has completed successfully.
     */
    virtual Status step(std::string_view inputData, std::string* outputData) = 0;

    virtual boost::optional<std::uint32_t> currentStep() const {
        return boost::none;
    }

    virtual boost::optional<std::uint32_t> totalSteps() const {
        return boost::none;
    }

    /**
     * Returns true if the authentication completed successfully.
     */
    virtual bool isSuccess() const = 0;

    /**
     * Returns the metrics recorder for this client session.
     * The session retains ownership of this pointer.
     */
    AuthMetricsRecorder* metrics() {
        return &_metricsRecorder;
    }

private:
    /**
     * Buffer object that owns data for a single parameter.
     */
    struct DataBuffer {
        std::unique_ptr<char[]> data;
        size_t size;
    };

    /// Buffers for each of the settable parameters.
    DataBuffer _parameters[numParameters];
    AuthMetricsRecorder _metricsRecorder;
};

}  // namespace mongo
