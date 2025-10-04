/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/SessionMode.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace S3
  {
    namespace Model
    {
      namespace SessionModeMapper
      {

        static const int ReadOnly_HASH = HashingUtils::HashString("ReadOnly");
        static const int ReadWrite_HASH = HashingUtils::HashString("ReadWrite");


        SessionMode GetSessionModeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == ReadOnly_HASH)
          {
            return SessionMode::ReadOnly;
          }
          else if (hashCode == ReadWrite_HASH)
          {
            return SessionMode::ReadWrite;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<SessionMode>(hashCode);
          }

          return SessionMode::NOT_SET;
        }

        Aws::String GetNameForSessionMode(SessionMode enumValue)
        {
          switch(enumValue)
          {
          case SessionMode::NOT_SET:
            return {};
          case SessionMode::ReadOnly:
            return "ReadOnly";
          case SessionMode::ReadWrite:
            return "ReadWrite";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace SessionModeMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
