/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/UpdateFunctionCodeRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/HashingUtils.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

UpdateFunctionCodeRequest::UpdateFunctionCodeRequest() : 
    m_functionNameHasBeenSet(false),
    m_zipFileHasBeenSet(false),
    m_s3BucketHasBeenSet(false),
    m_s3KeyHasBeenSet(false),
    m_s3ObjectVersionHasBeenSet(false),
    m_imageUriHasBeenSet(false),
    m_publish(false),
    m_publishHasBeenSet(false),
    m_dryRun(false),
    m_dryRunHasBeenSet(false),
    m_revisionIdHasBeenSet(false),
    m_architecturesHasBeenSet(false),
    m_sourceKMSKeyArnHasBeenSet(false)
{
}

Aws::String UpdateFunctionCodeRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_zipFileHasBeenSet)
  {
   payload.WithString("ZipFile", HashingUtils::Base64Encode(m_zipFile));
  }

  if(m_s3BucketHasBeenSet)
  {
   payload.WithString("S3Bucket", m_s3Bucket);

  }

  if(m_s3KeyHasBeenSet)
  {
   payload.WithString("S3Key", m_s3Key);

  }

  if(m_s3ObjectVersionHasBeenSet)
  {
   payload.WithString("S3ObjectVersion", m_s3ObjectVersion);

  }

  if(m_imageUriHasBeenSet)
  {
   payload.WithString("ImageUri", m_imageUri);

  }

  if(m_publishHasBeenSet)
  {
   payload.WithBool("Publish", m_publish);

  }

  if(m_dryRunHasBeenSet)
  {
   payload.WithBool("DryRun", m_dryRun);

  }

  if(m_revisionIdHasBeenSet)
  {
   payload.WithString("RevisionId", m_revisionId);

  }

  if(m_architecturesHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> architecturesJsonList(m_architectures.size());
   for(unsigned architecturesIndex = 0; architecturesIndex < architecturesJsonList.GetLength(); ++architecturesIndex)
   {
     architecturesJsonList[architecturesIndex].AsString(ArchitectureMapper::GetNameForArchitecture(m_architectures[architecturesIndex]));
   }
   payload.WithArray("Architectures", std::move(architecturesJsonList));

  }

  if(m_sourceKMSKeyArnHasBeenSet)
  {
   payload.WithString("SourceKMSKeyArn", m_sourceKMSKeyArn);

  }

  return payload.View().WriteReadable();
}




