/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/AnalyticsS3ExportFileFormat.h>
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
      namespace AnalyticsS3ExportFileFormatMapper
      {

        static const int CSV_HASH = HashingUtils::HashString("CSV");


        AnalyticsS3ExportFileFormat GetAnalyticsS3ExportFileFormatForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == CSV_HASH)
          {
            return AnalyticsS3ExportFileFormat::CSV;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<AnalyticsS3ExportFileFormat>(hashCode);
          }

          return AnalyticsS3ExportFileFormat::NOT_SET;
        }

        Aws::String GetNameForAnalyticsS3ExportFileFormat(AnalyticsS3ExportFileFormat enumValue)
        {
          switch(enumValue)
          {
          case AnalyticsS3ExportFileFormat::NOT_SET:
            return {};
          case AnalyticsS3ExportFileFormat::CSV:
            return "CSV";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace AnalyticsS3ExportFileFormatMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
