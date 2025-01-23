/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/EventSourceMappingMetric.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace Lambda
  {
    namespace Model
    {
      namespace EventSourceMappingMetricMapper
      {

        static const int EventCount_HASH = HashingUtils::HashString("EventCount");


        EventSourceMappingMetric GetEventSourceMappingMetricForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == EventCount_HASH)
          {
            return EventSourceMappingMetric::EventCount;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<EventSourceMappingMetric>(hashCode);
          }

          return EventSourceMappingMetric::NOT_SET;
        }

        Aws::String GetNameForEventSourceMappingMetric(EventSourceMappingMetric enumValue)
        {
          switch(enumValue)
          {
          case EventSourceMappingMetric::NOT_SET:
            return {};
          case EventSourceMappingMetric::EventCount:
            return "EventCount";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace EventSourceMappingMetricMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
