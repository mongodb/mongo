// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/client/sasl_client_conversation.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

namespace mongo {
/**
 *  Client side authentication session for SASL PLAIN.
 */
class SaslPLAINClientConversation : public SaslClientConversation {
    SaslPLAINClientConversation(const SaslPLAINClientConversation&) = delete;
    SaslPLAINClientConversation& operator=(const SaslPLAINClientConversation&) = delete;

public:
    /**
     * Implements the client side of a SASL PLAIN mechanism session.
     *
     **/
    explicit SaslPLAINClientConversation(SaslClientSession* saslClientSession);

    ~SaslPLAINClientConversation() override;

    StatusWith<bool> step(std::string_view inputData, std::string* outputData) override;

    boost::optional<std::uint32_t> currentStep() const override {
        return 1;
    }

    boost::optional<std::uint32_t> totalSteps() const override {
        return 1;
    }
};

}  // namespace mongo
