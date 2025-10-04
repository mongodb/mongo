/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/DeadLetterConfig.h>
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

DeadLetterConfig::DeadLetterConfig() : 
    m_targetArnHasBeenSet(false)
{
}

DeadLetterConfig::DeadLetterConfig(JsonView jsonValue)
  : DeadLetterConfig()
{
  *this = jsonValue;
}

DeadLetterConfig& DeadLetterConfig::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("TargetArn"))
  {
    m_targetArn = jsonValue.GetString("TargetArn");

    m_targetArnHasBeenSet = true;
  }

  return *this;
}

JsonValue DeadLetterConfig::Jsonize() const
{
  JsonValue payload;

  if(m_targetArnHasBeenSet)
  {
   payload.WithString("TargetArn", m_targetArn);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
