/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ReportFormatType.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace IAM
  {
    namespace Model
    {
      namespace ReportFormatTypeMapper
      {

        static const int text_csv_HASH = HashingUtils::HashString("text/csv");


        ReportFormatType GetReportFormatTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == text_csv_HASH)
          {
            return ReportFormatType::text_csv;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ReportFormatType>(hashCode);
          }

          return ReportFormatType::NOT_SET;
        }

        Aws::String GetNameForReportFormatType(ReportFormatType enumValue)
        {
          switch(enumValue)
          {
          case ReportFormatType::NOT_SET:
            return {};
          case ReportFormatType::text_csv:
            return "text/csv";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ReportFormatTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
