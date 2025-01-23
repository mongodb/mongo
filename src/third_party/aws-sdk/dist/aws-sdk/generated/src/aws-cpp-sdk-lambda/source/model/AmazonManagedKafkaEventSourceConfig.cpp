/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/AmazonManagedKafkaEventSourceConfig.h>
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

AmazonManagedKafkaEventSourceConfig::AmazonManagedKafkaEventSourceConfig() : 
    m_consumerGroupIdHasBeenSet(false)
{
}

AmazonManagedKafkaEventSourceConfig::AmazonManagedKafkaEventSourceConfig(JsonView jsonValue)
  : AmazonManagedKafkaEventSourceConfig()
{
  *this = jsonValue;
}

AmazonManagedKafkaEventSourceConfig& AmazonManagedKafkaEventSourceConfig::operator =(JsonView jsonValue)
{
  if(jsonValue.ValueExists("ConsumerGroupId"))
  {
    m_consumerGroupId = jsonValue.GetString("ConsumerGroupId");

    m_consumerGroupIdHasBeenSet = true;
  }

  return *this;
}

JsonValue AmazonManagedKafkaEventSourceConfig::Jsonize() const
{
  JsonValue payload;

  if(m_consumerGroupIdHasBeenSet)
  {
   payload.WithString("ConsumerGroupId", m_consumerGroupId);

  }

  return payload;
}

} // namespace Model
} // namespace Lambda
} // namespace Aws
