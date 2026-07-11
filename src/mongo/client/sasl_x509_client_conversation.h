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
 *  Client side authentication session for SASL X509.
 */
class SaslX509ClientConversation : public SaslClientConversation {
    SaslX509ClientConversation(const SaslX509ClientConversation&) = delete;
    SaslX509ClientConversation& operator=(const SaslX509ClientConversation&) = delete;

public:
    /**
     * Implements the client side of a SASL X509 mechanism session.
     *
     **/
    explicit SaslX509ClientConversation(SaslClientSession* saslClientSession);

    StatusWith<bool> step(std::string_view inputData, std::string* outputData) override;

    boost::optional<std::uint32_t> currentStep() const override {
        return 1;
    }

    boost::optional<std::uint32_t> totalSteps() const override {
        return 1;
    }
};

}  // namespace mongo
