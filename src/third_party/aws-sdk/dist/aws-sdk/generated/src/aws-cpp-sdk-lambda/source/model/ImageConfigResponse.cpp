/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ImageConfigResponse.h>
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

ImageConfigResponse::ImageConfigResponse() : 
    m_imageConfigHasBeenSet(false),
    m_errorHasBeenSet(false)
{
}

ImageConfigResponse::ImageConfigResponse(JsonView jsonValue)
  : ImageConfigResponse()
{
  *this = jsonValue;
}

ImageConfigResponse& ImageConfigResponse::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("ImageConfig"))
  {
    m_imageConfig = jsonValue.GetObject("ImageConfig");

    m_imageConfigHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Error"))
  {
    m_error = jsonValue.GetObject("Error");

    m_errorHasBeenSet = true;
  }

  return *this;
}

JsonValue ImageConfigResponse::Jsonize() const
{
  JsonValue payload;

  if(m_imageConfigHasBeenSet)
  {
   payload.WithObject("ImageConfig", m_imageConfig.Jsonize());

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
