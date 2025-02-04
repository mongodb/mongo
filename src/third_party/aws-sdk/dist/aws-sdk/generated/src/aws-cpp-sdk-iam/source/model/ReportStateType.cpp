/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/ReportStateType.h>
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
      namespace ReportStateTypeMapper
      {

        static const int STARTED_HASH = HashingUtils::HashString("STARTED");
        static const int INPROGRESS_HASH = HashingUtils::HashString("INPROGRESS");
        static const int COMPLETE_HASH = HashingUtils::HashString("COMPLETE");


        ReportStateType GetReportStateTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == STARTED_HASH)
          {
            return ReportStateType::STARTED;
          }
          else if (hashCode == INPROGRESS_HASH)
          {
            return ReportStateType::INPROGRESS;
          }
          else if (hashCode == COMPLETE_HASH)
          {
            return ReportStateType::COMPLETE;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ReportStateType>(hashCode);
          }

          return ReportStateType::NOT_SET;
        }

        Aws::String GetNameForReportStateType(ReportStateType enumValue)
        {
          switch(enumValue)
          {
          case ReportStateType::NOT_SET:
            return {};
          case ReportStateType::STARTED:
            return "STARTED";
          case ReportStateType::INPROGRESS:
            return "INPROGRESS";
          case ReportStateType::COMPLETE:
            return "COMPLETE";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ReportStateTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
