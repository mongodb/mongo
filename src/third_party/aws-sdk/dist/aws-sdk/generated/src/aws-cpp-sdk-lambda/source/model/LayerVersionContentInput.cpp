/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/LayerVersionContentInput.h>
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

LayerVersionContentInput::LayerVersionContentInput() : 
    m_s3BucketHasBeenSet(false),
    m_s3KeyHasBeenSet(false),
    m_s3ObjectVersionHasBeenSet(false),
    m_zipFileHasBeenSet(false)
{
}

LayerVersionContentInput::LayerVersionContentInput(JsonView jsonValue)
  : LayerVersionContentInput()
{
  *this = jsonValue;
}

LayerVersionContentInput& LayerVersionContentInput::operator =(JsonView jsonValue)
{
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

  if(jsonValue.ValueExists("ZipFile"))
  {
    m_zipFile = HashingUtils::Base64Decode(jsonValue.GetString("ZipFile"));
    m_zipFileHasBeenSet = true;
  }

  return *this;
}

JsonValue LayerVersionContentInput::Jsonize() const
{
  JsonValue payload;

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

  if(m_zipFileHasBeenSet)
  {
   payload.WithString("ZipFile", HashingUtils::Base64Encode(m_zipFile));
  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
