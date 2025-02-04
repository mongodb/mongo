/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/Environment.h>
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

Environment::Environment() : 
    m_variablesHasBeenSet(false)
{
}

Environment::Environment(JsonView jsonValue)
  : Environment()
{
  *this = jsonValue;
}

Environment& Environment::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Variables"))
  {
    Aws::Map<Aws::String, JsonView> variablesJsonMap = jsonValue.GetObject("Variables").GetAllObjects();
    for(auto& variablesItem : variablesJsonMap)
    {
      m_variables[variablesItem.first] = variablesItem.second.AsString();
    }
    m_variablesHasBeenSet = true;
  }

  return *this;
}

JsonValue Environment::Jsonize() const
{
  JsonValue payload;

  if(m_variablesHasBeenSet)
  {
   JsonValue variablesJsonMap;
   for(auto& variablesItem : m_variables)
   {
     variablesJsonMap.WithString(variablesItem.first, variablesItem.second);
   }
   payload.WithObject("Variables", std::move(variablesJsonMap));

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
