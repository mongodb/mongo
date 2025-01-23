/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/CreateEventSourceMappingRequest.h>
#include <aws/core/utils/json/JsonSerializer.h>

#include <utility>

using namespace Aws::Lambda::Model;
using namespace Aws::Utils::Json;
using namespace Aws::Utils;

CreateEventSourceMappingRequest::CreateEventSourceMappingRequest() : 
    m_eventSourceArnHasBeenSet(false),
    m_functionNameHasBeenSet(false),
    m_enabled(false),
    m_enabledHasBeenSet(false),
    m_batchSize(0),
    m_batchSizeHasBeenSet(false),
    m_filterCriteriaHasBeenSet(false),
    m_maximumBatchingWindowInSeconds(0),
    m_maximumBatchingWindowInSecondsHasBeenSet(false),
    m_parallelizationFactor(0),
    m_parallelizationFactorHasBeenSet(false),
    m_startingPosition(EventSourcePosition::NOT_SET),
    m_startingPositionHasBeenSet(false),
    m_startingPositionTimestampHasBeenSet(false),
    m_destinationConfigHasBeenSet(false),
    m_maximumRecordAgeInSeconds(0),
    m_maximumRecordAgeInSecondsHasBeenSet(false),
    m_bisectBatchOnFunctionError(false),
    m_bisectBatchOnFunctionErrorHasBeenSet(false),
    m_maximumRetryAttempts(0),
    m_maximumRetryAttemptsHasBeenSet(false),
    m_tagsHasBeenSet(false),
    m_tumblingWindowInSeconds(0),
    m_tumblingWindowInSecondsHasBeenSet(false),
    m_topicsHasBeenSet(false),
    m_queuesHasBeenSet(false),
    m_sourceAccessConfigurationsHasBeenSet(false),
    m_selfManagedEventSourceHasBeenSet(false),
    m_functionResponseTypesHasBeenSet(false),
    m_amazonManagedKafkaEventSourceConfigHasBeenSet(false),
    m_selfManagedKafkaEventSourceConfigHasBeenSet(false),
    m_scalingConfigHasBeenSet(false),
    m_documentDBEventSourceConfigHasBeenSet(false),
    m_kMSKeyArnHasBeenSet(false),
    m_metricsConfigHasBeenSet(false),
    m_provisionedPollerConfigHasBeenSet(false)
{
}

Aws::String CreateEventSourceMappingRequest::SerializePayload() const
{
  JsonValue payload;

  if(m_eventSourceArnHasBeenSet)
  {
   payload.WithString("EventSourceArn", m_eventSourceArn);

  }

  if(m_functionNameHasBeenSet)
  {
   payload.WithString("FunctionName", m_functionName);

  }

  if(m_enabledHasBeenSet)
  {
   payload.WithBool("Enabled", m_enabled);

  }

  if(m_batchSizeHasBeenSet)
  {
   payload.WithInteger("BatchSize", m_batchSize);

  }

  if(m_filterCriteriaHasBeenSet)
  {
   payload.WithObject("FilterCriteria", m_filterCriteria.Jsonize());

  }

  if(m_maximumBatchingWindowInSecondsHasBeenSet)
  {
   payload.WithInteger("MaximumBatchingWindowInSeconds", m_maximumBatchingWindowInSeconds);

  }

  if(m_parallelizationFactorHasBeenSet)
  {
   payload.WithInteger("ParallelizationFactor", m_parallelizationFactor);

  }

  if(m_startingPositionHasBeenSet)
  {
   payload.WithString("StartingPosition", EventSourcePositionMapper::GetNameForEventSourcePosition(m_startingPosition));
  }

  if(m_startingPositionTimestampHasBeenSet)
  {
   payload.WithDouble("StartingPositionTimestamp", m_startingPositionTimestamp.SecondsWithMSPrecision());
  }

  if(m_destinationConfigHasBeenSet)
  {
   payload.WithObject("DestinationConfig", m_destinationConfig.Jsonize());

  }

  if(m_maximumRecordAgeInSecondsHasBeenSet)
  {
   payload.WithInteger("MaximumRecordAgeInSeconds", m_maximumRecordAgeInSeconds);

  }

  if(m_bisectBatchOnFunctionErrorHasBeenSet)
  {
   payload.WithBool("BisectBatchOnFunctionError", m_bisectBatchOnFunctionError);

  }

  if(m_maximumRetryAttemptsHasBeenSet)
  {
   payload.WithInteger("MaximumRetryAttempts", m_maximumRetryAttempts);

  }

  if(m_tagsHasBeenSet)
  {
   JsonValue tagsJsonMap;
   for(auto& tagsItem : m_tags)
   {
     tagsJsonMap.WithString(tagsItem.first, tagsItem.second);
   }
   payload.WithObject("Tags", std::move(tagsJsonMap));

  }

  if(m_tumblingWindowInSecondsHasBeenSet)
  {
   payload.WithInteger("TumblingWindowInSeconds", m_tumblingWindowInSeconds);

  }

  if(m_topicsHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> topicsJsonList(m_topics.size());
   for(unsigned topicsIndex = 0; topicsIndex < topicsJsonList.GetLength(); ++topicsIndex)
   {
     topicsJsonList[topicsIndex].AsString(m_topics[topicsIndex]);
   }
   payload.WithArray("Topics", std::move(topicsJsonList));

  }

  if(m_queuesHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> queuesJsonList(m_queues.size());
   for(unsigned queuesIndex = 0; queuesIndex < queuesJsonList.GetLength(); ++queuesIndex)
   {
     queuesJsonList[queuesIndex].AsString(m_queues[queuesIndex]);
   }
   payload.WithArray("Queues", std::move(queuesJsonList));

  }

  if(m_sourceAccessConfigurationsHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> sourceAccessConfigurationsJsonList(m_sourceAccessConfigurations.size());
   for(unsigned sourceAccessConfigurationsIndex = 0; sourceAccessConfigurationsIndex < sourceAccessConfigurationsJsonList.GetLength(); ++sourceAccessConfigurationsIndex)
   {
     sourceAccessConfigurationsJsonList[sourceAccessConfigurationsIndex].AsObject(m_sourceAccessConfigurations[sourceAccessConfigurationsIndex].Jsonize());
   }
   payload.WithArray("SourceAccessConfigurations", std::move(sourceAccessConfigurationsJsonList));

  }

  if(m_selfManagedEventSourceHasBeenSet)
  {
   payload.WithObject("SelfManagedEventSource", m_selfManagedEventSource.Jsonize());

  }

  if(m_functionResponseTypesHasBeenSet)
  {
   Aws::Utils::Array<JsonValue> functionResponseTypesJsonList(m_functionResponseTypes.size());
   for(unsigned functionResponseTypesIndex = 0; functionResponseTypesIndex < functionResponseTypesJsonList.GetLength(); ++functionResponseTypesIndex)
   {
     functionResponseTypesJsonList[functionResponseTypesIndex].AsString(FunctionResponseTypeMapper::GetNameForFunctionResponseType(m_functionResponseTypes[functionResponseTypesIndex]));
   }
   payload.WithArray("FunctionResponseTypes", std::move(functionResponseTypesJsonList));

  }

  if(m_amazonManagedKafkaEventSourceConfigHasBeenSet)
  {
   payload.WithObject("AmazonManagedKafkaEventSourceConfig", m_amazonManagedKafkaEventSourceConfig.Jsonize());

  }

  if(m_selfManagedKafkaEventSourceConfigHasBeenSet)
  {
   payload.WithObject("SelfManagedKafkaEventSourceConfig", m_selfManagedKafkaEventSourceConfig.Jsonize());

  }

  if(m_scalingConfigHasBeenSet)
  {
   payload.WithObject("ScalingConfig", m_scalingConfig.Jsonize());

  }

  if(m_documentDBEventSourceConfigHasBeenSet)
  {
   payload.WithObject("DocumentDBEventSourceConfig", m_documentDBEventSourceConfig.Jsonize());

  }

  if(m_kMSKeyArnHasBeenSet)
  {
   payload.WithString("KMSKeyArn", m_kMSKeyArn);

  }

  if(m_metricsConfigHasBeenSet)
  {
   payload.WithObject("MetricsConfig", m_metricsConfig.Jsonize());

  }

  if(m_provisionedPollerConfigHasBeenSet)
  {
   payload.WithObject("ProvisionedPollerConfig", m_provisionedPollerConfig.Jsonize());

  }

  return payload.View().WriteReadable();
}




