/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/Concurrency.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>

#include <utility>

using namespace Aws::Utils::Json;
using namespace Aws::Utils;

namespace Aws
{
namespace Lambda
{
namespace Model
{

Concurrency::Concurrency() : 
    m_reservedConcurrentExecutions(0),
    m_reservedConcurrentExecutionsHasBeenSet(false),
    m_requestIdHasBeenSet(false)
{
}

Concurrency::Concurrency(JsonView jsonValue)
  : Concurrency()
{
  *this = jsonValue;
}

Concurrency& Concurrency::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("ReservedConcurrentExecutions"))
  {
    m_reservedConcurrentExecutions = jsonValue.GetInteger("ReservedConcurrentExecutions");

    m_reservedConcurrentExecutionsHasBeenSet = true;
  }

  return *this;
}

JsonValue Concurrency::Jsonize() const
{
  JsonValue payload;

  if(m_reservedConcurrentExecutionsHasBeenSet)
  {
   payload.WithInteger("ReservedConcurrentExecutions", m_reservedConcurrentExecutions);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
