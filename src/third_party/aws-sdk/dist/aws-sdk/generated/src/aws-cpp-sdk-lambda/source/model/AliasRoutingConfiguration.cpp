/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/AliasRoutingConfiguration.h>
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

AliasRoutingConfiguration::AliasRoutingConfiguration() : 
    m_additionalVersionWeightsHasBeenSet(false)
{
}

AliasRoutingConfiguration::AliasRoutingConfiguration(JsonView jsonValue)
  : AliasRoutingConfiguration()
{
  *this = jsonValue;
}

AliasRoutingConfiguration& AliasRoutingConfiguration::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("AdditionalVersionWeights"))
  {
    Aws::Map<Aws::String, JsonView> additionalVersionWeightsJsonMap = jsonValue.GetObject("AdditionalVersionWeights").GetAllObjects();
    for(auto& additionalVersionWeightsItem : additionalVersionWeightsJsonMap)
    {
      m_additionalVersionWeights[additionalVersionWeightsItem.first] = additionalVersionWeightsItem.second.AsDouble();
    }
    m_additionalVersionWeightsHasBeenSet = true;
  }

  return *this;
}

JsonValue AliasRoutingConfiguration::Jsonize() const
{
  JsonValue payload;

  if(m_additionalVersionWeightsHasBeenSet)
  {
   JsonValue additionalVersionWeightsJsonMap;
   for(auto& additionalVersionWeightsItem : m_additionalVersionWeights)
   {
     additionalVersionWeightsJsonMap.WithDouble(additionalVersionWeightsItem.first, additionalVersionWeightsItem.second);
   }
   payload.WithObject("AdditionalVersionWeights", std::move(additionalVersionWeightsJsonMap));

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
