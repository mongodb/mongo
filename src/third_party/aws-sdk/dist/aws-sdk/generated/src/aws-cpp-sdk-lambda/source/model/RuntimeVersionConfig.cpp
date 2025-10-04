/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/RuntimeVersionConfig.h>
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

RuntimeVersionConfig::RuntimeVersionConfig() : 
    m_runtimeVersionArnHasBeenSet(false),
    m_errorHasBeenSet(false)
{
}

RuntimeVersionConfig::RuntimeVersionConfig(JsonView jsonValue)
  : RuntimeVersionConfig()
{
  *this = jsonValue;
}

RuntimeVersionConfig& RuntimeVersionConfig::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("RuntimeVersionArn"))
  {
    m_runtimeVersionArn = jsonValue.GetString("RuntimeVersionArn");

    m_runtimeVersionArnHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Error"))
  {
    m_error = jsonValue.GetObject("Error");

    m_errorHasBeenSet = true;
  }

  return *this;
}

JsonValue RuntimeVersionConfig::Jsonize() const
{
  JsonValue payload;

  if(m_runtimeVersionArnHasBeenSet)
  {
   payload.WithString("RuntimeVersionArn", m_runtimeVersionArn);

  }

  if(m_errorHasBeenSet)
  {
   payload.WithObject("Error", m_error.Jsonize());

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
