/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ImageConfig.h>
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

ImageConfig::ImageConfig() : 
    m_entryPointHasBeenSet(false),
    m_commandHasBeenSet(false),
    m_workingDirectoryHasBeenSet(false)
{
}

ImageConfig::ImageConfig(JsonView jsonValue)
  : ImageConfig()
{
  *this = jsonValue;
}

ImageConfig& ImageConfig::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("EntryPoint"))
  {
    Aws::Utils::Array<JsonView> entryPointJsonList = jsonValue.GetArray("EntryPoint");
    for(unsigned entryPointIndex = 0; entryPointIndex < entryPointJsonList.GetLength(); ++entryPointIndex)
    {
      m_entryPoint.push_back(entryPointJsonList[entryPointIndex].AsString());
    }
    m_entryPointHasBeenSet = true;
  }

  if(jsonValue.ValueExists("Command"))
  {
    Aws::Utils::Array<JsonView> commandJsonList = jsonValue.GetArray("Command");
    for(unsigned commandIndex = 0; commandIndex < commandJsonList.GetLength(); ++commandIndex)
    {
      m_command.push_back(commandJsonList[commandIndex].AsString());
    }
    m_commandHasBeenSet = true;
  }

  if(jsonValue.ValueExists("WorkingDirectory"))
  {
    m_workingDirectory = jsonValue.GetString("WorkingDirectory");

    m_workingDirectoryHasBeenSet = true;
  }

  return *this;
}

JsonValue ImageConfig::Jsonize() const
{
  JsonValue payload;

  if(m_entryPointHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> entryPointJsonList(m_entryPoint.size());
   for(unsigned entryPointIndex = 0; entryPointIndex < entryPointJsonList.GetLength(); ++entryPointIndex)
   {
     entryPointJsonList[entryPointIndex].AsString(m_entryPoint[entryPointIndex]);
   }
   payload.WithArray("EntryPoint", std::move(entryPointJsonList));

  }

  if(m_commandHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> commandJsonList(m_command.size());
   for(unsigned commandIndex = 0; commandIndex < commandJsonList.GetLength(); ++commandIndex)
   {
     commandJsonList[commandIndex].AsString(m_command[commandIndex]);
   }
   payload.WithArray("Command", std::move(commandJsonList));

  }

  if(m_workingDirectoryHasBeenSet)
  {
   payload.WithString("WorkingDirectory", m_workingDirectory);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
