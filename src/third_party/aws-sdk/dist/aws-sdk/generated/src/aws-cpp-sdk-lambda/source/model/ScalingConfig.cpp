/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ScalingConfig.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Lambda
{
namespace Model
{

ScalingConfig::ScalingConfig() : 
    m_maximumConcurrency(0),
    m_maximumConcurrencyHasBeenSet(false)
{
}

ScalingConfig::ScalingConfig(JsonView jsonValue)
  : ScalingConfig()
{
  *this = jsonValue;
}

ScalingConfig& ScalingConfig::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("MaximumConcurrency"))
  {
    m_maximumConcurrency = jsonValue.GetInteger("MaximumConcurrency");

    m_maximumConcurrencyHasBeenSet = true;
  }

  return *this;
}

JsonValue ScalingConfig::Jsonize() const
{
  JsonValue payload;

  if(m_maximumConcurrencyHasBeenSet)
  {
   payload.WithInteger("MaximumConcurrency", m_maximumConcurrency);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
