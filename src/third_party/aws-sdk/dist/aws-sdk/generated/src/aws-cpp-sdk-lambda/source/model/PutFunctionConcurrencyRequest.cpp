/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/PutFunctionConcurrencyRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

PutFunctionConcurrencyRequest::PutFunctionConcurrencyRequest() : 
    m_functionNameHasBeenSet(false),
    m_reservedConcurrentExecutions(0),
    m_reservedConcurrentExecutionsHasBeenSet(false)
{
}

Aws::String PutFunctionConcurrencyRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_reservedConcurrentExecutionsHasBeenSet)
  {
   payload.WithInteger("ReservedConcurrentExecutions", m_reservedConcurrentExecutions);

  }

  return payload.View().WriteReadable();
}




