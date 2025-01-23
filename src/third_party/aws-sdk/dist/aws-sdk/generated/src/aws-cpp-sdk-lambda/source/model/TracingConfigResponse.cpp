/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/TracingConfigResponse.h>
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

TracingConfigResponse::TracingConfigResponse() : 
    m_mode(TracingMode::NOT_SET),
    m_modeHasBeenSet(false)
{
}

TracingConfigResponse::TracingConfigResponse(JsonView jsonValue)
  : TracingConfigResponse()
{
  *this = jsonValue;
}

TracingConfigResponse& TracingConfigResponse::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Mode"))
  {
    m_mode = TracingModeMapper::GetTracingModeForName(jsonValue.GetString("Mode"));

    m_modeHasBeenSet = true;
  }

  return *this;
}

JsonValue TracingConfigResponse::Jsonize() const
{
  JsonValue payload;

  if(m_modeHasBeenSet)
  {
   payload.WithString("Mode", TracingModeMapper::GetNameForTracingMode(m_mode));
  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
