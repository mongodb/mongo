// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] awsIam {
/**
 * SASL AWS Client parameters
 */
struct SASLAwsClientGlobalParams {
    /**
     * EC2 Instance metadata endpoint.
     */
    std::string awsEC2InstanceMetadataUrl;

    /**
     * ECS Instance metadata endpoint.
     */
    std::string awsECSInstanceMetadataUrl;

    /**
     * AWS Session Token.
     */
    std::string awsSessionToken;
};

extern SASLAwsClientGlobalParams saslAwsClientGlobalParams;
}  // namespace awsIam
}  // namespace mongo
