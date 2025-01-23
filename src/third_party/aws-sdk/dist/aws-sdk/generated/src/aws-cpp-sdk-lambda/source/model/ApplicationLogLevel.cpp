/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ApplicationLogLevel.h>
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
      namespace ApplicationLogLevelMapper
      {

        static const int TRACE_HASH = HashingUtils::HashString("TRACE");
        static const int DEBUG__HASH = HashingUtils::HashString("DEBUG");
        static const int INFO_HASH = HashingUtils::HashString("INFO");
        static const int WARN_HASH = HashingUtils::HashString("WARN");
        static const int ERROR__HASH = HashingUtils::HashString("ERROR");
        static const int FATAL_HASH = HashingUtils::HashString("FATAL");


        ApplicationLogLevel GetApplicationLogLevelForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == TRACE_HASH)
          {
            return ApplicationLogLevel::TRACE;
          }
          else if (hashCode == DEBUG__HASH)
          {
            return ApplicationLogLevel::DEBUG_;
          }
          else if (hashCode == INFO_HASH)
          {
            return ApplicationLogLevel::INFO;
          }
          else if (hashCode == WARN_HASH)
          {
            return ApplicationLogLevel::WARN;
          }
          else if (hashCode == ERROR__HASH)
          {
            return ApplicationLogLevel::ERROR_;
          }
          else if (hashCode == FATAL_HASH)
          {
            return ApplicationLogLevel::FATAL;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ApplicationLogLevel>(hashCode);
          }

          return ApplicationLogLevel::NOT_SET;
        }

        Aws::String GetNameForApplicationLogLevel(ApplicationLogLevel enumValue)
        {
          switch(enumValue)
          {
          case ApplicationLogLevel::NOT_SET:
            return {};
          case ApplicationLogLevel::TRACE:
            return "TRACE";
          case ApplicationLogLevel::DEBUG_:
            return "DEBUG";
          case ApplicationLogLevel::INFO:
            return "INFO";
          case ApplicationLogLevel::WARN:
            return "WARN";
          case ApplicationLogLevel::ERROR_:
            return "ERROR";
          case ApplicationLogLevel::FATAL:
            return "FATAL";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ApplicationLogLevelMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
