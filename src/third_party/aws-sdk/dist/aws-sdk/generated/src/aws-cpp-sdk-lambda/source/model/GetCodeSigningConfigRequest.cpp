/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/GetCodeSigningConfigRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

GetCodeSigningConfigRequest::GetCodeSigningConfigRequest() : 
    m_codeSigningConfigArnHasBeenSet(false)
{
}

Aws::String GetCodeSigningConfigRequest::SerializePayload() const
{
  return {};
}




