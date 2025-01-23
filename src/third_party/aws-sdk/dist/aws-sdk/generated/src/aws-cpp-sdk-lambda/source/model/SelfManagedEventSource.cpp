/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/SelfManagedEventSource.h>
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

SelfManagedEventSource::SelfManagedEventSource() : 
    m_endpointsHasBeenSet(false)
{
}

SelfManagedEventSource::SelfManagedEventSource(JsonView jsonValue)
  : SelfManagedEventSource()
{
  *this = jsonValue;
}

SelfManagedEventSource& SelfManagedEventSource::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Endpoints"))
  {
    Aws::Map<Aws::String, JsonView> endpointsJsonMap = jsonValue.GetObject("Endpoints").GetAllObjects();
    for(auto& endpointsItem : endpointsJsonMap)
    {
      Aws::Utils::Array<JsonView> endpointListsJsonList = endpointsItem.second.AsArray();
      Aws::Vector<Aws::String> endpointListsList;
      endpointListsList.reserve((size_t)endpointListsJsonList.GetLength());
      for(unsigned endpointListsIndex = 0; endpointListsIndex < endpointListsJsonList.GetLength(); ++endpointListsIndex)
      {
        endpointListsList.push_back(endpointListsJsonList[endpointListsIndex].AsString());
      }
      m_endpoints[EndPointTypeMapper::GetEndPointTypeForName(endpointsItem.first)] = std::move(endpointListsList);
    }
    m_endpointsHasBeenSet = true;
  }

  return *this;
}

JsonValue SelfManagedEventSource::Jsonize() const
{
  JsonValue payload;

  if(m_endpointsHasBeenSet)
  {
   JsonValue endpointsJsonMap;
   for(auto& endpointsItem : m_endpoints)
   {
     Aws::Utils::Array<JsonValue> endpointListsJsonList(endpointsItem.second.size());
     for(unsigned endpointListsIndex = 0; endpointListsIndex < endpointListsJsonList.GetLength(); ++endpointListsIndex)
     {
       endpointListsJsonList[endpointListsIndex].AsString(endpointsItem.second[endpointListsIndex]);
     }
     endpointsJsonMap.WithArray(EndPointTypeMapper::GetNameForEndPointType(endpointsItem.first), std::move(endpointListsJsonList));
   }
   payload.WithObject("Endpoints", std::move(endpointsJsonMap));

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
