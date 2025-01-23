/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/Layer.h>
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

Layer::Layer() : 
    m_arnHasBeenSet(false),
    m_codeSize(0),
    m_codeSizeHasBeenSet(false),
    m_signingProfileVersionArnHasBeenSet(false),
    m_signingJobArnHasBeenSet(false)
{
}

Layer::Layer(JsonView jsonValue)
  : Layer()
{
  *this = jsonValue;
}

Layer& Layer::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Arn"))
  {
    m_arn = jsonValue.GetString("Arn");

    m_arnHasBeenSet = true;
  }

  if(jsonValue.ValueExists("CodeSize"))
  {
    m_codeSize = jsonValue.GetInt64("CodeSize");

    m_codeSizeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("SigningProfileVersionArn"))
  {
    m_signingProfileVersionArn = jsonValue.GetString("SigningProfileVersionArn");

    m_signingProfileVersionArnHasBeenSet = true;
  }

  if(jsonValue.ValueExists("SigningJobArn"))
  {
    m_signingJobArn = jsonValue.GetString("SigningJobArn");

    m_signingJobArnHasBeenSet = true;
  }

  return *this;
}

JsonValue Layer::Jsonize() const
{
  JsonValue payload;

  if(m_arnHasBeenSet)
  {
   payload.WithString("Arn", m_arn);

  }

  if(m_codeSizeHasBeenSet)
  {
   payload.WithInt64("CodeSize", m_codeSize);

  }

  if(m_signingProfileVersionArnHasBeenSet)
  {
   payload.WithString("SigningProfileVersionArn", m_signingProfileVersionArn);

  }

  if(m_signingJobArnHasBeenSet)
  {
   payload.WithString("SigningJobArn", m_signingJobArn);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
