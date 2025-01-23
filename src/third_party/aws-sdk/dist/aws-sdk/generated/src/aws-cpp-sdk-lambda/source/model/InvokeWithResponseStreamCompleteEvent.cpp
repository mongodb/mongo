/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/InvokeWithResponseStreamCompleteEvent.h>
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

InvokeWithResponseStreamCompleteEvent::InvokeWithResponseStreamCompleteEvent() : 
    m_errorCodeHasBeenSet(false),
    m_errorDetailsHasBeenSet(false),
    m_logResultHasBeenSet(false)
{
}

InvokeWithResponseStreamCompleteEvent::InvokeWithResponseStreamCompleteEvent(JsonView jsonValue)
  : InvokeWithResponseStreamCompleteEvent()
{
  *this = jsonValue;
}

InvokeWithResponseStreamCompleteEvent& InvokeWithResponseStreamCompleteEvent::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("ErrorCode"))
  {
    m_errorCode = jsonValue.GetString("ErrorCode");

    m_errorCodeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ErrorDetails"))
  {
    m_errorDetails = jsonValue.GetString("ErrorDetails");

    m_errorDetailsHasBeenSet = true;
  }

  if(jsonValue.ValueExists("LogResult"))
  {
    m_logResult = jsonValue.GetString("LogResult");

    m_logResultHasBeenSet = true;
  }

  return *this;
}

JsonValue InvokeWithResponseStreamCompleteEvent::Jsonize() const
{
  JsonValue payload;

  if(m_errorCodeHasBeenSet)
  {
   payload.WithString("ErrorCode", m_errorCode);

  }

  if(m_errorDetailsHasBeenSet)
  {
   payload.WithString("ErrorDetails", m_errorDetails);

  }

  if(m_logResultHasBeenSet)
  {
   payload.WithString("LogResult", m_logResult);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
