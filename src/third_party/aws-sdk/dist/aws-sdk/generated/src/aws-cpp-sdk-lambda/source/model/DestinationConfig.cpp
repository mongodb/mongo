/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/DestinationConfig.h>
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

DestinationConfig::DestinationConfig() : 
    m_onSuccessHasBeenSet(false),
    m_onFailureHasBeenSet(false)
{
}

DestinationConfig::DestinationConfig(JsonView jsonValue)
  : DestinationConfig()
{
  *this = jsonValue;
}

DestinationConfig& DestinationConfig::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("OnSuccess"))
  {
    m_onSuccess = jsonValue.GetObject("OnSuccess");

    m_onSuccessHasBeenSet = true;
  }

  if(jsonValue.ValueExists("OnFailure"))
  {
    m_onFailure = jsonValue.GetObject("OnFailure");

    m_onFailureHasBeenSet = true;
  }

  return *this;
}

JsonValue DestinationConfig::Jsonize() const
{
  JsonValue payload;

  if(m_onSuccessHasBeenSet)
  {
   payload.WithObject("OnSuccess", m_onSuccess.Jsonize());

  }

  if(m_onFailureHasBeenSet)
  {
   payload.WithObject("OnFailure", m_onFailure.Jsonize());

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
