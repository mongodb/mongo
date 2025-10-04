/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/AccountLimit.h>
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

AccountLimit::AccountLimit() : 
    m_totalCodeSize(0),
    m_totalCodeSizeHasBeenSet(false),
    m_codeSizeUnzipped(0),
    m_codeSizeUnzippedHasBeenSet(false),
    m_codeSizeZipped(0),
    m_codeSizeZippedHasBeenSet(false),
    m_concurrentExecutions(0),
    m_concurrentExecutionsHasBeenSet(false),
    m_unreservedConcurrentExecutions(0),
    m_unreservedConcurrentExecutionsHasBeenSet(false)
{
}

AccountLimit::AccountLimit(JsonView jsonValue)
  : AccountLimit()
{
  *this = jsonValue;
}

AccountLimit& AccountLimit::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("TotalCodeSize"))
  {
    m_totalCodeSize = jsonValue.GetInt64("TotalCodeSize");

    m_totalCodeSizeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("CodeSizeUnzipped"))
  {
    m_codeSizeUnzipped = jsonValue.GetInt64("CodeSizeUnzipped");

    m_codeSizeUnzippedHasBeenSet = true;
  }

  if(jsonValue.ValueExists("CodeSizeZipped"))
  {
    m_codeSizeZipped = jsonValue.GetInt64("CodeSizeZipped");

    m_codeSizeZippedHasBeenSet = true;
  }

  if(jsonValue.ValueExists("ConcurrentExecutions"))
  {
    m_concurrentExecutions = jsonValue.GetInteger("ConcurrentExecutions");

    m_concurrentExecutionsHasBeenSet = true;
  }

  if(jsonValue.ValueExists("UnreservedConcurrentExecutions"))
  {
    m_unreservedConcurrentExecutions = jsonValue.GetInteger("UnreservedConcurrentExecutions");

    m_unreservedConcurrentExecutionsHasBeenSet = true;
  }

  return *this;
}

JsonValue AccountLimit::Jsonize() const
{
  JsonValue payload;

  if(m_totalCodeSizeHasBeenSet)
  {
   payload.WithInt64("TotalCodeSize", m_totalCodeSize);

  }

  if(m_codeSizeUnzippedHasBeenSet)
  {
   payload.WithInt64("CodeSizeUnzipped", m_codeSizeUnzipped);

  }

  if(m_codeSizeZippedHasBeenSet)
  {
   payload.WithInt64("CodeSizeZipped", m_codeSizeZipped);

  }

  if(m_concurrentExecutionsHasBeenSet)
  {
   payload.WithInteger("ConcurrentExecutions", m_concurrentExecutions);

  }

  if(m_unreservedConcurrentExecutionsHasBeenSet)
  {
   payload.WithInteger("UnreservedConcurrentExecutions", m_unreservedConcurrentExecutions);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
