/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/PublishVersionRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

PublishVersionRequest::PublishVersionRequest() : 
    m_functionNameHasBeenSet(false),
    m_codeSha256HasBeenSet(false),
    m_descriptionHasBeenSet(false),
    m_revisionIdHasBeenSet(false)
{
}

Aws::String PublishVersionRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_codeSha256HasBeenSet)
  {
   payload.WithString("CodeSha256", m_codeSha256);

  }

  if(m_descriptionHasBeenSet)
  {
   payload.WithString("Description", m_description);

  }

  if(m_revisionIdHasBeenSet)
  {
   payload.WithString("RevisionId", m_revisionId);

  }

  return payload.View().WriteReadable();
}




