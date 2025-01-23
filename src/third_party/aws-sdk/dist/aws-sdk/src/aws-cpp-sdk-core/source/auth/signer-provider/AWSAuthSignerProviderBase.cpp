/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/core/auth/signer-provider/AWSAuthSignerProviderBase.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>

using namespace Aws::Auth;

std::shared_ptr<AWSCredentialsProvider> AWSAuthSignerProvider::GetCredentialsProvider() const {
  return MakeShared<DefaultAWSCredentialsProviderChain>("AWSAuthSignerProvider");
}
