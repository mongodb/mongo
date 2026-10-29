// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once


#include "mongo/base/status.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo {

class SaslClientConversation;

/**
 * Implementation of the client side of a SASL authentication conversation using the
 * native SASL implementation.
 */
class [[MONGO_MOD_PUBLIC]] NativeSaslClientSession : public SaslClientSession {
    NativeSaslClientSession(const NativeSaslClientSession&) = delete;
    NativeSaslClientSession& operator=(const NativeSaslClientSession&) = delete;

public:
    NativeSaslClientSession();
    ~NativeSaslClientSession() override;

    Status initialize() override;

    Status step(std::string_view inputData, std::string* outputData) override;

    boost::optional<std::uint32_t> currentStep() const override;

    boost::optional<std::uint32_t> totalSteps() const override;

    bool isSuccess() const override {
        return _success;
    }

private:
    /// See isSuccess().
    bool _success;

    /// The client side of a SASL authentication conversation.
    std::unique_ptr<SaslClientConversation> _saslConversation;
};

}  // namespace mongo
