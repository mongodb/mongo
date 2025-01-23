/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/StreamMode.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace Kinesis
  {
    namespace Model
    {
      namespace StreamModeMapper
      {

        static const int PROVISIONED_HASH = HashingUtils::HashString("PROVISIONED");
        static const int ON_DEMAND_HASH = HashingUtils::HashString("ON_DEMAND");


        StreamMode GetStreamModeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == PROVISIONED_HASH)
          {
            return StreamMode::PROVISIONED;
          }
          else if (hashCode == ON_DEMAND_HASH)
          {
            return StreamMode::ON_DEMAND;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<StreamMode>(hashCode);
          }

          return StreamMode::NOT_SET;
        }

        Aws::String GetNameForStreamMode(StreamMode enumValue)
        {
          switch(enumValue)
          {
          case StreamMode::NOT_SET:
            return {};
          case StreamMode::PROVISIONED:
            return "PROVISIONED";
          case StreamMode::ON_DEMAND:
            return "ON_DEMAND";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace StreamModeMapper
    } // namespace Model
  } // namespace Kinesis
} // namespace Aws
