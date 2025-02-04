/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/SnapStart.h>
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

SnapStart::SnapStart() : 
    m_applyOn(SnapStartApplyOn::NOT_SET),
    m_applyOnHasBeenSet(false)
{
}

SnapStart::SnapStart(JsonView jsonValue)
  : SnapStart()
{
  *this = jsonValue;
}

SnapStart& SnapStart::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("ApplyOn"))
  {
    m_applyOn = SnapStartApplyOnMapper::GetSnapStartApplyOnForName(jsonValue.GetString("ApplyOn"));

    m_applyOnHasBeenSet = true;
  }

  return *this;
}

JsonValue SnapStart::Jsonize() const
{
  JsonValue payload;

  if(m_applyOnHasBeenSet)
  {
   payload.WithString("ApplyOn", SnapStartApplyOnMapper::GetNameForSnapStartApplyOn(m_applyOn));
  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
