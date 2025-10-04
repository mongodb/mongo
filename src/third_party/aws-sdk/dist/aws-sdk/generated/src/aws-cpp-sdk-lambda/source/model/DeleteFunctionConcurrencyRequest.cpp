/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/DeleteFunctionConcurrencyRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

DeleteFunctionConcurrencyRequest::DeleteFunctionConcurrencyRequest() : 
    m_functionNameHasBeenSet(false)
{
}

Aws::String DeleteFunctionConcurrencyRequest::SerializePayload() const
{
  return {};
}




