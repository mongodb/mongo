/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/FileHeaderInfo.h>
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
      namespace FileHeaderInfoMapper
      {

        static const int USE_HASH = HashingUtils::HashString("USE");
        static const int IGNORE_HASH = HashingUtils::HashString("IGNORE");
        static const int NONE_HASH = HashingUtils::HashString("NONE");


        FileHeaderInfo GetFileHeaderInfoForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == USE_HASH)
          {
            return FileHeaderInfo::USE;
          }
          else if (hashCode == IGNORE_HASH)
          {
            return FileHeaderInfo::IGNORE;
          }
          else if (hashCode == NONE_HASH)
          {
            return FileHeaderInfo::NONE;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<FileHeaderInfo>(hashCode);
          }

          return FileHeaderInfo::NOT_SET;
        }

        Aws::String GetNameForFileHeaderInfo(FileHeaderInfo enumValue)
        {
          switch(enumValue)
          {
          case FileHeaderInfo::NOT_SET:
            return {};
          case FileHeaderInfo::USE:
            return "USE";
          case FileHeaderInfo::IGNORE:
            return "IGNORE";
          case FileHeaderInfo::NONE:
            return "NONE";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace FileHeaderInfoMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
