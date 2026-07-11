// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/sasl_aws_protocol_common.h"
#include "mongo/client/sasl_client_conversation.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {
/**
 *  Client side authentication session for MONGODB-AWS SASL.
 */
class SaslAWSClientConversation : public SaslClientConversation {
    SaslAWSClientConversation(const SaslAWSClientConversation&) = delete;
    SaslAWSClientConversation& operator=(const SaslAWSClientConversation&) = delete;

public:
    explicit SaslAWSClientConversation(SaslClientSession* saslClientSession);

    ~SaslAWSClientConversation() override = default;

    StatusWith<bool> step(std::string_view inputData, std::string* outputData) override;

    boost::optional<std::uint32_t> currentStep() const override {
        return _step;
    }

    boost::optional<std::uint32_t> totalSteps() const override {
        return _maxStep;
    }

private:
    /**
     * Get AWS credentials either from the SASL session or a local HTTP server.
     */
    awsIam::AWSCredentials _getCredentials() const;

    /**
     * Get AWS credentials from SASL session.
     */
    awsIam::AWSCredentials _getUserCredentials() const;

    /**
     * Get AWS credentials from local HTTP server.
     */
    awsIam::AWSCredentials _getLocalAWSCredentials() const;

    /**
     * Get AWS credentials from EC2 Instance metadata HTTP server.
     */
    awsIam::AWSCredentials _getEc2Credentials() const;

    /**
     * Get AWS credentials from ECS Instance metadata HTTP server.
     */
    awsIam::AWSCredentials _getEcsCredentials(std::string_view relativeUri) const;

    StatusWith<bool> _firstStep(std::string* outputData);
    StatusWith<bool> _secondStep(std::string_view inputData, std::string* outputData);

private:
    // Step of protocol - either 1 or 2
    std::uint32_t _step{0};
    const std::uint32_t _maxStep = 2;

    // Client nonce
    std::vector<char> _clientNonce;
};

}  // namespace mongo
