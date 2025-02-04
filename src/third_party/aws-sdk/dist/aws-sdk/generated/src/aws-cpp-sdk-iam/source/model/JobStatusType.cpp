/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/JobStatusType.h>
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
      namespace JobStatusTypeMapper
      {

        static const int IN_PROGRESS_HASH = HashingUtils::HashString("IN_PROGRESS");
        static const int COMPLETED_HASH = HashingUtils::HashString("COMPLETED");
        static const int FAILED_HASH = HashingUtils::HashString("FAILED");


        JobStatusType GetJobStatusTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == IN_PROGRESS_HASH)
          {
            return JobStatusType::IN_PROGRESS;
          }
          else if (hashCode == COMPLETED_HASH)
          {
            return JobStatusType::COMPLETED;
          }
          else if (hashCode == FAILED_HASH)
          {
            return JobStatusType::FAILED;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<JobStatusType>(hashCode);
          }

          return JobStatusType::NOT_SET;
        }

        Aws::String GetNameForJobStatusType(JobStatusType enumValue)
        {
          switch(enumValue)
          {
          case JobStatusType::NOT_SET:
            return {};
          case JobStatusType::IN_PROGRESS:
            return "IN_PROGRESS";
          case JobStatusType::COMPLETED:
            return "COMPLETED";
          case JobStatusType::FAILED:
            return "FAILED";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace JobStatusTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
