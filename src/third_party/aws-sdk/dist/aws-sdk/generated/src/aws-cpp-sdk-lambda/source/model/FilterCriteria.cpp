/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/FilterCriteria.h>
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

FilterCriteria::FilterCriteria() : 
    m_filtersHasBeenSet(false)
{
}

FilterCriteria::FilterCriteria(JsonView jsonValue)
  : FilterCriteria()
{
  *this = jsonValue;
}

FilterCriteria& FilterCriteria::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Filters"))
  {
    Aws::Utils::Array<JsonView> filtersJsonList = jsonValue.GetArray("Filters");
    for(unsigned filtersIndex = 0; filtersIndex < filtersJsonList.GetLength(); ++filtersIndex)
    {
      m_filters.push_back(filtersJsonList[filtersIndex].AsObject());
    }
    m_filtersHasBeenSet = true;
  }

  return *this;
}

JsonValue FilterCriteria::Jsonize() const
{
  JsonValue payload;

  if(m_filtersHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> filtersJsonList(m_filters.size());
   for(unsigned filtersIndex = 0; filtersIndex < filtersJsonList.GetLength(); ++filtersIndex)
   {
     filtersJsonList[filtersIndex].AsObject(m_filters[filtersIndex].Jsonize());
   }
   payload.WithArray("Filters", std::move(filtersJsonList));

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
