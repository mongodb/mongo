/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/SourceAccessConfiguration.h>
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

SourceAccessConfiguration::SourceAccessConfiguration() : 
    m_type(SourceAccessType::NOT_SET),
    m_typeHasBeenSet(false),
    m_uRIHasBeenSet(false)
{
}

SourceAccessConfiguration::SourceAccessConfiguration(JsonView jsonValue)
  : SourceAccessConfiguration()
{
  *this = jsonValue;
}

SourceAccessConfiguration& SourceAccessConfiguration::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("Type"))
  {
    m_type = SourceAccessTypeMapper::GetSourceAccessTypeForName(jsonValue.GetString("Type"));

    m_typeHasBeenSet = true;
  }

  if(jsonValue.ValueExists("URI"))
  {
    m_uRI = jsonValue.GetString("URI");

    m_uRIHasBeenSet = true;
  }

  return *this;
}

JsonValue SourceAccessConfiguration::Jsonize() const
{
  JsonValue payload;

  if(m_typeHasBeenSet)
  {
   payload.WithString("Type", SourceAccessTypeMapper::GetNameForSourceAccessType(m_type));
  }

  if(m_uRIHasBeenSet)
  {
   payload.WithString("URI", m_uRI);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
