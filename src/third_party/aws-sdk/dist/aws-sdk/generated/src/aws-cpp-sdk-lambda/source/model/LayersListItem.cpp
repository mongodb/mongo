/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/LayersListItem.h>
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

LayersListItem::LayersListItem() : 
    m_layerNameHasBeenSet(false),
    m_layerArnHasBeenSet(false),
    m_latestMatchingVersionHasBeenSet(false)
{
}

LayersListItem::LayersListItem(JsonView jsonValue)
  : LayersListItem()
{
  *this = jsonValue;
}

LayersListItem& LayersListItem::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("LayerName"))
  {
    m_layerName = jsonValue.GetString("LayerName");

    m_layerNameHasBeenSet = true;
  }

  if(jsonValue.ValueExists("LayerArn"))
  {
    m_layerArn = jsonValue.GetString("LayerArn");

    m_layerArnHasBeenSet = true;
  }

  if(jsonValue.ValueExists("LatestMatchingVersion"))
  {
    m_latestMatchingVersion = jsonValue.GetObject("LatestMatchingVersion");

    m_latestMatchingVersionHasBeenSet = true;
  }

  return *this;
}

JsonValue LayersListItem::Jsonize() const
{
  JsonValue payload;

  if(m_layerNameHasBeenSet)
  {
   payload.WithString("LayerName", m_layerName);

  }

  if(m_layerArnHasBeenSet)
  {
   payload.WithString("LayerArn", m_layerArn);

  }

  if(m_latestMatchingVersionHasBeenSet)
  {
   payload.WithObject("LatestMatchingVersion", m_latestMatchingVersion.Jsonize());

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
