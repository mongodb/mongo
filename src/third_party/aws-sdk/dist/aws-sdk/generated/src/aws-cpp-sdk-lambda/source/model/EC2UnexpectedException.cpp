/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/EC2UnexpectedException.h>
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

EC2UnexpectedException::EC2UnexpectedException() : 
    m_typeHasBeenSet(false),
    m_messageHasBeenSet(false),
    m_eC2ErrorCodeHasBeenSet(false)
{
}

EC2UnexpectedException::EC2UnexpectedException(JsonView jsonValue)
  : EC2UnexpectedException()
{
  *this = jsonValue;
}

EC2UnexpectedException& EC2UnexpectedException::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Type"))
  {
    m_type = jsonValue.GetString("Type");

    m_typeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Message"))
  {
    m_message = jsonValue.GetString("Message");

    m_messageHasBeenSet = true;
  }

  if(jsonValue.ValueExists("EC2ErrorCode"))
  {
    m_eC2ErrorCode = jsonValue.GetString("EC2ErrorCode");

    m_eC2ErrorCodeHasBeenSet = true;
  }

  return *this;
}

JsonValue EC2UnexpectedException::Jsonize() const
{
  JsonValue payload;

  if(m_typeHasBeenSet)
  {
   payload.WithString("Type", m_type);

  }

  if(m_messageHasBeenSet)
  {
   payload.WithString("Message", m_message);

  }

  if(m_eC2ErrorCodeHasBeenSet)
  {
   payload.WithString("EC2ErrorCode", m_eC2ErrorCode);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
