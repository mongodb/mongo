/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/AccountUsage.h>
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

AccountUsage::AccountUsage() : 
    m_totalCodeSize(0),
    m_totalCodeSizeHasBeenSet(false),
    m_functionCount(0),
    m_functionCountHasBeenSet(false)
{
}

AccountUsage::AccountUsage(JsonView jsonValue)
  : AccountUsage()
{
  *this = jsonValue;
}

AccountUsage& AccountUsage::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("TotalCodeSize"))
  {
    m_totalCodeSize = jsonValue.GetInt64("TotalCodeSize");

    m_totalCodeSizeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("FunctionCount"))
  {
    m_functionCount = jsonValue.GetInt64("FunctionCount");

    m_functionCountHasBeenSet = true;
  }

  return *this;
}

JsonValue AccountUsage::Jsonize() const
{
  JsonValue payload;

  if(m_totalCodeSizeHasBeenSet)
  {
   payload.WithInt64("TotalCodeSize", m_totalCodeSize);

  }

  if(m_functionCountHasBeenSet)
  {
   payload.WithInt64("FunctionCount", m_functionCount);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
