/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/GetFunctionCodeSigningConfigRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

GetFunctionCodeSigningConfigRequest::GetFunctionCodeSigningConfigRequest() : 
    m_functionNameHasBeenSet(false)
{
}

Aws::String GetFunctionCodeSigningConfigRequest::SerializePayload() const
{
  return {};
}




