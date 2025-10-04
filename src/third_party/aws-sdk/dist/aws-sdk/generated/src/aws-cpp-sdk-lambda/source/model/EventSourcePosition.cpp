/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/EventSourcePosition.h>
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
      namespace EventSourcePositionMapper
      {

        static const int TRIM_HORIZON_HASH = HashingUtils::HashString("TRIM_HORIZON");
        static const int LATEST_HASH = HashingUtils::HashString("LATEST");
        static const int AT_TIMESTAMP_HASH = HashingUtils::HashString("AT_TIMESTAMP");


        EventSourcePosition GetEventSourcePositionForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == TRIM_HORIZON_HASH)
          {
            return EventSourcePosition::TRIM_HORIZON;
          }
          else if (hashCode == LATEST_HASH)
          {
            return EventSourcePosition::LATEST;
          }
          else if (hashCode == AT_TIMESTAMP_HASH)
          {
            return EventSourcePosition::AT_TIMESTAMP;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<EventSourcePosition>(hashCode);
          }

          return EventSourcePosition::NOT_SET;
        }

        Aws::String GetNameForEventSourcePosition(EventSourcePosition enumValue)
        {
          switch(enumValue)
          {
          case EventSourcePosition::NOT_SET:
            return {};
          case EventSourcePosition::TRIM_HORIZON:
            return "TRIM_HORIZON";
          case EventSourcePosition::LATEST:
            return "LATEST";
          case EventSourcePosition::AT_TIMESTAMP:
            return "AT_TIMESTAMP";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace EventSourcePositionMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
