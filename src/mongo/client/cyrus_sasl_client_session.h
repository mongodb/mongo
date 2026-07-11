// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/client/sasl_client_session.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <sasl/sasl.h>

namespace mongo {

/**
 * Implementation of the client side of a SASL authentication conversation.
 * using the Cyrus SASL library.
 */
class CyrusSaslClientSession : public SaslClientSession {
    CyrusSaslClientSession(const CyrusSaslClientSession&) = delete;
    CyrusSaslClientSession& operator=(const CyrusSaslClientSession&) = delete;

public:
    CyrusSaslClientSession();
    ~CyrusSaslClientSession() override;

    /**
     * Overriding to store the password data in sasl_secret_t format
     */
    void setParameter(Parameter id, std::string_view value) override;

    /**
     * Returns the value of the parameterPassword parameter in the form of a sasl_secret_t, used
     * by the Cyrus SASL library's SASL_CB_PASS callback.  The session object owns the storage
     * referenced by the returned sasl_secret_t*, which will remain in scope according to the
     * same rules as given for SaslClientSession::getParameter().
     */
    sasl_secret_t* getPasswordAsSecret();

    Status initialize() override;

    Status step(std::string_view inputData, std::string* outputData) override;

    boost::optional<std::uint32_t> currentStep() const override;

    boost::optional<std::uint32_t> totalSteps() const override;

    bool isSuccess() const override {
        return _success;
    }

private:
    /// Maximum number of Cyrus SASL callbacks stored in _callbacks.
    static const int maxCallbacks = 4;

    /// Underlying Cyrus SASL library connection object.
    sasl_conn_t* _saslConnection;

    // Number of successfully completed conversation steps.
    std::uint32_t _step;

    /// See isSuccess().
    bool _success;

    /// Stored of password in sasl_secret_t format
    std::unique_ptr<char[]> _secret;

    /// Callbacks registered on _saslConnection for providing the Cyrus SASL library with
    /// parameter values, etc.
    sasl_callback_t _callbacks[maxCallbacks];
};

}  // namespace mongo
