// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>
#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

class SaslClientSession;
template <typename T>
class StatusWith;

/**
 * Abstract class for implementing the client-side
 * of a SASL mechanism conversation.
 */
class [[MONGO_MOD_OPEN]] SaslClientConversation {
    SaslClientConversation(const SaslClientConversation&) = delete;
    SaslClientConversation& operator=(const SaslClientConversation&) = delete;

public:
    /**
     * Implements the client side of a SASL authentication mechanism.
     *
     * "saslClientSession" is the corresponding SASLClientSession.
     * "saslClientSession" must stay in scope until the SaslClientConversation's
     *  destructor completes.
     *
     **/
    explicit SaslClientConversation(SaslClientSession* saslClientSession)
        : _saslClientSession(saslClientSession) {}

    virtual ~SaslClientConversation();

    /**
     * Performs one step of the client side of the authentication session,
     * consuming "inputData" and producing "*outputData".
     *
     * A return of Status::OK() indicates successful progress towards authentication.
     * A return of !Status::OK() indicates failed authentication
     *
     * A return of true means that the authentication process has finished.
     * A return of false means that the authentication process has more steps.
     *
     */
    virtual StatusWith<bool> step(std::string_view inputData, std::string* outputData) = 0;

    virtual boost::optional<std::uint32_t> currentStep() const {
        return boost::none;
    }
    virtual boost::optional<std::uint32_t> totalSteps() const {
        return boost::none;
    }

protected:
    SaslClientSession* _saslClientSession;
};

}  // namespace mongo
