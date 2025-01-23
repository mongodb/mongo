/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/FileSystemConfig.h>
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

FileSystemConfig::FileSystemConfig() : 
    m_arnHasBeenSet(false),
    m_localMountPathHasBeenSet(false)
{
}

FileSystemConfig::FileSystemConfig(JsonView jsonValue)
  : FileSystemConfig()
{
  *this = jsonValue;
}

FileSystemConfig& FileSystemConfig::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Arn"))
  {
    m_arn = jsonValue.GetString("Arn");

    m_arnHasBeenSet = true;
  }

  if(jsonValue.ValueExists("LocalMountPath"))
  {
    m_localMountPath = jsonValue.GetString("LocalMountPath");

    m_localMountPathHasBeenSet = true;
  }

  return *this;
}

JsonValue FileSystemConfig::Jsonize() const
{
  JsonValue payload;

  if(m_arnHasBeenSet)
  {
   payload.WithString("Arn", m_arn);

  }

  if(m_localMountPathHasBeenSet)
  {
   payload.WithString("LocalMountPath", m_localMountPath);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
