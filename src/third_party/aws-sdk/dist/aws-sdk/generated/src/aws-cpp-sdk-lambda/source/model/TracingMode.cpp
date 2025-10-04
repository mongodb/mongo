/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/TracingMode.h>
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
      namespace TracingModeMapper
      {

        static const int Active_HASH = HashingUtils::HashString("Active");
        static const int PassThrough_HASH = HashingUtils::HashString("PassThrough");


        TracingMode GetTracingModeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Active_HASH)
          {
            return TracingMode::Active;
          }
          else if (hashCode == PassThrough_HASH)
          {
            return TracingMode::PassThrough;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<TracingMode>(hashCode);
          }

          return TracingMode::NOT_SET;
        }

        Aws::String GetNameForTracingMode(TracingMode enumValue)
        {
          switch(enumValue)
          {
          case TracingMode::NOT_SET:
            return {};
          case TracingMode::Active:
            return "Active";
          case TracingMode::PassThrough:
            return "PassThrough";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace TracingModeMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
