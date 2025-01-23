/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/SystemLogLevel.h>
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
      namespace SystemLogLevelMapper
      {

        static const int DEBUG__HASH = HashingUtils::HashString("DEBUG");
        static const int INFO_HASH = HashingUtils::HashString("INFO");
        static const int WARN_HASH = HashingUtils::HashString("WARN");


        SystemLogLevel GetSystemLogLevelForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == DEBUG__HASH)
          {
            return SystemLogLevel::DEBUG_;
          }
          else if (hashCode == INFO_HASH)
          {
            return SystemLogLevel::INFO;
          }
          else if (hashCode == WARN_HASH)
          {
            return SystemLogLevel::WARN;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<SystemLogLevel>(hashCode);
          }

          return SystemLogLevel::NOT_SET;
        }

        Aws::String GetNameForSystemLogLevel(SystemLogLevel enumValue)
        {
          switch(enumValue)
          {
          case SystemLogLevel::NOT_SET:
            return {};
          case SystemLogLevel::DEBUG_:
            return "DEBUG";
          case SystemLogLevel::INFO:
            return "INFO";
          case SystemLogLevel::WARN:
            return "WARN";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace SystemLogLevelMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
