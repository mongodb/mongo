/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/FunctionCodeLocation.h>
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

FunctionCodeLocation::FunctionCodeLocation() : 
    m_repositoryTypeHasBeenSet(false),
    m_locationHasBeenSet(false),
    m_imageUriHasBeenSet(false),
    m_resolvedImageUriHasBeenSet(false),
    m_sourceKMSKeyArnHasBeenSet(false)
{
}

FunctionCodeLocation::FunctionCodeLocation(JsonView jsonValue)
  : FunctionCodeLocation()
{
  *this = jsonValue;
}

FunctionCodeLocation& FunctionCodeLocation::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("RepositoryType"))
  {
    m_repositoryType = jsonValue.GetString("RepositoryType");

    m_repositoryTypeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Location"))
  {
    m_location = jsonValue.GetString("Location");

    m_locationHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ImageUri"))
  {
    m_imageUri = jsonValue.GetString("ImageUri");

    m_imageUriHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ResolvedImageUri"))
  {
    m_resolvedImageUri = jsonValue.GetString("ResolvedImageUri");

    m_resolvedImageUriHasBeenSet = true;
  }

  if(jsonValue.ValueExists("SourceKMSKeyArn"))
  {
    m_sourceKMSKeyArn = jsonValue.GetString("SourceKMSKeyArn");

    m_sourceKMSKeyArnHasBeenSet = true;
  }

  return *this;
}

JsonValue FunctionCodeLocation::Jsonize() const
{
  JsonValue payload;

  if(m_repositoryTypeHasBeenSet)
  {
   payload.WithString("RepositoryType", m_repositoryType);

  }

  if(m_locationHasBeenSet)
  {
   payload.WithString("Location", m_location);

  }

  if(m_imageUriHasBeenSet)
  {
   payload.WithString("ImageUri", m_imageUri);

  }

  if(m_resolvedImageUriHasBeenSet)
  {
   payload.WithString("ResolvedImageUri", m_resolvedImageUri);

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
