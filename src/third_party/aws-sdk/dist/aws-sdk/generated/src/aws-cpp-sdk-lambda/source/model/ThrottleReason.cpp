/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ThrottleReason.h>
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
      namespace ThrottleReasonMapper
      {

        static const int ConcurrentInvocationLimitExceeded_HASH = HashingUtils::HashString("ConcurrentInvocationLimitExceeded");
        static const int FunctionInvocationRateLimitExceeded_HASH = HashingUtils::HashString("FunctionInvocationRateLimitExceeded");
        static const int ReservedFunctionConcurrentInvocationLimitExceeded_HASH = HashingUtils::HashString("ReservedFunctionConcurrentInvocationLimitExceeded");
        static const int ReservedFunctionInvocationRateLimitExceeded_HASH = HashingUtils::HashString("ReservedFunctionInvocationRateLimitExceeded");
        static const int CallerRateLimitExceeded_HASH = HashingUtils::HashString("CallerRateLimitExceeded");
        static const int ConcurrentSnapshotCreateLimitExceeded_HASH = HashingUtils::HashString("ConcurrentSnapshotCreateLimitExceeded");


        ThrottleReason GetThrottleReasonForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == ConcurrentInvocationLimitExceeded_HASH)
          {
            return ThrottleReason::ConcurrentInvocationLimitExceeded;
          }
          else if (hashCode == FunctionInvocationRateLimitExceeded_HASH)
          {
            return ThrottleReason::FunctionInvocationRateLimitExceeded;
          }
          else if (hashCode == ReservedFunctionConcurrentInvocationLimitExceeded_HASH)
          {
            return ThrottleReason::ReservedFunctionConcurrentInvocationLimitExceeded;
          }
          else if (hashCode == ReservedFunctionInvocationRateLimitExceeded_HASH)
          {
            return ThrottleReason::ReservedFunctionInvocationRateLimitExceeded;
          }
          else if (hashCode == CallerRateLimitExceeded_HASH)
          {
            return ThrottleReason::CallerRateLimitExceeded;
          }
          else if (hashCode == ConcurrentSnapshotCreateLimitExceeded_HASH)
          {
            return ThrottleReason::ConcurrentSnapshotCreateLimitExceeded;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ThrottleReason>(hashCode);
          }

          return ThrottleReason::NOT_SET;
        }

        Aws::String GetNameForThrottleReason(ThrottleReason enumValue)
        {
          switch(enumValue)
          {
          case ThrottleReason::NOT_SET:
            return {};
          case ThrottleReason::ConcurrentInvocationLimitExceeded:
            return "ConcurrentInvocationLimitExceeded";
          case ThrottleReason::FunctionInvocationRateLimitExceeded:
            return "FunctionInvocationRateLimitExceeded";
          case ThrottleReason::ReservedFunctionConcurrentInvocationLimitExceeded:
            return "ReservedFunctionConcurrentInvocationLimitExceeded";
          case ThrottleReason::ReservedFunctionInvocationRateLimitExceeded:
            return "ReservedFunctionInvocationRateLimitExceeded";
          case ThrottleReason::CallerRateLimitExceeded:
            return "CallerRateLimitExceeded";
          case ThrottleReason::ConcurrentSnapshotCreateLimitExceeded:
            return "ConcurrentSnapshotCreateLimitExceeded";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ThrottleReasonMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
