/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/LogFormat.h>
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
      namespace LogFormatMapper
      {

        static const int JSON_HASH = HashingUtils::HashString("JSON");
        static const int Text_HASH = HashingUtils::HashString("Text");


        LogFormat GetLogFormatForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == JSON_HASH)
          {
            return LogFormat::JSON;
          }
          else if (hashCode == Text_HASH)
          {
            return LogFormat::Text;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<LogFormat>(hashCode);
          }

          return LogFormat::NOT_SET;
        }

        Aws::String GetNameForLogFormat(LogFormat enumValue)
        {
          switch(enumValue)
          {
          case LogFormat::NOT_SET:
            return {};
          case LogFormat::JSON:
            return "JSON";
          case LogFormat::Text:
            return "Text";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace LogFormatMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
