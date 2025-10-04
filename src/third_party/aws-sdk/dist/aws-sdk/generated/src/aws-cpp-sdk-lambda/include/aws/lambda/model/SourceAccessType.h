/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/lambda/Lambda_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace Lambda
{
namespace Model
{
  enum class SourceAccessType
  {
    NOT_SET,
    BASIC_AUTH,
    VPC_SUBNET,
    VPC_SECURITY_GROUP,
    SASL_SCRAM_512_AUTH,
    SASL_SCRAM_256_AUTH,
    VIRTUAL_HOST,
    CLIENT_CERTIFICATE_TLS_AUTH,
    SERVER_ROOT_CA_CERTIFICATE
  };

namespace SourceAccessTypeMapper
{
AWS_LAMBDA_API SourceAccessType GetSourceAccessTypeForName(const Aws::String& name);

AWS_LAMBDA_API Aws::String GetNameForSourceAccessType(SourceAccessType value);
} // namespace SourceAccessTypeMapper
} // namespace Model
} // namespace Lambda
} // namespace Aws
