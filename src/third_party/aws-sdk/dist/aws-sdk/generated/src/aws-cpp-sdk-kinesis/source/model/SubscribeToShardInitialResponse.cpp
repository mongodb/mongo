/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/SubscribeToShardInitialResponse.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/UnreferencedParam.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Kinesis
{
namespace Model
{

SubscribeToShardInitialResponse::SubscribeToShardInitialResponse()
{
}

SubscribeToShardInitialResponse::SubscribeToShardInitialResponse(JsonView jsonValue)
{
  *this = jsonValue;
}

SubscribeToShardInitialResponse& SubscribeToShardInitialResponse::operator =(JsonView jsonValue)
{
  AWS_UNREFERENCED_PARAM(jsonValue);
  return *this;
}

SubscribeToShardInitialResponse::SubscribeToShardInitialResponse(const Http::HeaderValueCollection& headers) : SubscribeToShardInitialResponse()
{
  AWS_UNREFERENCED_PARAM(headers);
}

JsonValue SubscribeToShardInitialResponse::Jsonize() const
{
  JsonValue payload;

  return payload;
}

} // namespace Model
} // namespace Kinesis
} // namespace Aws
