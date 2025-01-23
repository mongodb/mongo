/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/SnapStartResponse.h>
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

SnapStartResponse::SnapStartResponse() : 
    m_applyOn(SnapStartApplyOn::NOT_SET),
    m_applyOnHasBeenSet(false),
    m_optimizationStatus(SnapStartOptimizationStatus::NOT_SET),
    m_optimizationStatusHasBeenSet(false)
{
}

SnapStartResponse::SnapStartResponse(JsonView jsonValue)
  : SnapStartResponse()
{
  *this = jsonValue;
}

SnapStartResponse& SnapStartResponse::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("ApplyOn"))
  {
    m_applyOn = SnapStartApplyOnMapper::GetSnapStartApplyOnForName(jsonValue.GetString("ApplyOn"));

    m_applyOnHasBeenSet = true;
  }

  if(jsonValue.ValueExists("OptimizationStatus"))
  {
    m_optimizationStatus = SnapStartOptimizationStatusMapper::GetSnapStartOptimizationStatusForName(jsonValue.GetString("OptimizationStatus"));

    m_optimizationStatusHasBeenSet = true;
  }

  return *this;
}

JsonValue SnapStartResponse::Jsonize() const
{
  JsonValue payload;

  if(m_applyOnHasBeenSet)
  {
   payload.WithString("ApplyOn", SnapStartApplyOnMapper::GetNameForSnapStartApplyOn(m_applyOn));
  }

  if(m_optimizationStatusHasBeenSet)
  {
   payload.WithString("OptimizationStatus", SnapStartOptimizationStatusMapper::GetNameForSnapStartOptimizationStatus(m_optimizationStatus));
  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
