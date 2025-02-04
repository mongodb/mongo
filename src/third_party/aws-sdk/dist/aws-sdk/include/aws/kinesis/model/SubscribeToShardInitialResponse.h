/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/kinesis/Kinesis_EXPORTS.h>
#include <aws/core/http/HttpTypes.h>

namespace Aws
{
namespace Utils
{
namespace Json
{
  class JsonValue;
  class JsonView;
} // namespace Json
} // namespace Utils
namespace Kinesis
{
namespace Model
{

  class SubscribeToShardInitialResponse
  {
  public:
    AWS_KINESIS_API SubscribeToShardInitialResponse();
    AWS_KINESIS_API SubscribeToShardInitialResponse(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API SubscribeToShardInitialResponse& operator=(Aws::Utils::Json::JsonView jsonValue);
    AWS_KINESIS_API SubscribeToShardInitialResponse(const Http::HeaderValueCollection& responseHeaders);
    AWS_KINESIS_API Aws::Utils::Json::JsonValue Jsonize() const;

  };

} // namespace Model
} // namespace Kinesis
} // namespace Aws
