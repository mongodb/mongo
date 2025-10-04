/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/FunctionCode.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/HashingUtils.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Lambda
{
namespace Model
{

FunctionCode::FunctionCode() : 
    m_zipFileHasBeenSet(false),
    m_s3BucketHasBeenSet(false),
    m_s3KeyHasBeenSet(false),
    m_s3ObjectVersionHasBeenSet(false),
    m_imageUriHasBeenSet(false),
    m_sourceKMSKeyArnHasBeenSet(false)
{
}

FunctionCode::FunctionCode(JsonView jsonValue)
  : FunctionCode()
{
  *this = jsonValue;
}

FunctionCode& FunctionCode::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("ZipFile"))
  {
    m_zipFile = HashingUtils::Base64Decode(jsonValue.GetString("ZipFile"));
    m_zipFileHasBeenSet = true;
  }

  if(jsonValue.ValueExists("S3Bucket"))
  {
    m_s3Bucket = jsonValue.GetString("S3Bucket");

    m_s3BucketHasBeenSet = true;
  }

  if(jsonValue.ValueExists("S3Key"))
  {
    m_s3Key = jsonValue.GetString("S3Key");

    m_s3KeyHasBeenSet = true;
  }

  if(jsonValue.ValueExists("S3ObjectVersion"))
  {
    m_s3ObjectVersion = jsonValue.GetString("S3ObjectVersion");

    m_s3ObjectVersionHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ImageUri"))
  {
    m_imageUri = jsonValue.GetString("ImageUri");

    m_imageUriHasBeenSet = true;
  }

  if(jsonValue.ValueExists("SourceKMSKeyArn"))
  {
    m_sourceKMSKeyArn = jsonValue.GetString("SourceKMSKeyArn");

    m_sourceKMSKeyArnHasBeenSet = true;
  }

  return *this;
}

JsonValue FunctionCode::Jsonize() const
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

  if(m_sourceKMSKeyArnHasBeenSet)
  {
   payload.WithString("SourceKMSKeyArn", m_sourceKMSKeyArn);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
