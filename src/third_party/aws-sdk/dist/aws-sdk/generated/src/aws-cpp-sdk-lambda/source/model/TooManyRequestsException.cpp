/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/TooManyRequestsException.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Lambda
{
namespace Model
{

TooManyRequestsException::TooManyRequestsException() : 
    m_retryAfterSecondsHasBeenSet(false),
    m_typeHasBeenSet(false),
    m_messageHasBeenSet(false),
    m_reason(ThrottleReason::NOT_SET),
    m_reasonHasBeenSet(false)
{
}

TooManyRequestsException::TooManyRequestsException(JsonView jsonValue)
  : TooManyRequestsException()
{
  *this = jsonValue;
}

TooManyRequestsException& TooManyRequestsException::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Type"))
  {
    m_type = jsonValue.GetString("Type");

    m_typeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("message"))
  {
    m_message = jsonValue.GetString("message");

    m_messageHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Reason"))
  {
    m_reason = ThrottleReasonMapper::GetThrottleReasonForName(jsonValue.GetString("Reason"));

    m_reasonHasBeenSet = true;
  }

  return *this;
}

JsonValue TooManyRequestsException::Jsonize() const
{
  JsonValue payload;

  if(m_typeHasBeenSet)
  {
   payload.WithString("Type", m_type);

  }

  if(m_messageHasBeenSet)
  {
   payload.WithString("message", m_message);

  }

  if(m_reasonHasBeenSet)
  {
   payload.WithString("Reason", ThrottleReasonMapper::GetNameForThrottleReason(m_reason));
  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
