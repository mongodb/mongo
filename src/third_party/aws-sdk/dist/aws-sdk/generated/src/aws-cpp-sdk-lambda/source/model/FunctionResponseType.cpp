/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/FunctionResponseType.h>
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
      namespace FunctionResponseTypeMapper
      {

        static const int ReportBatchItemFailures_HASH = HashingUtils::HashString("ReportBatchItemFailures");


        FunctionResponseType GetFunctionResponseTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == ReportBatchItemFailures_HASH)
          {
            return FunctionResponseType::ReportBatchItemFailures;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<FunctionResponseType>(hashCode);
          }

          return FunctionResponseType::NOT_SET;
        }

        Aws::String GetNameForFunctionResponseType(FunctionResponseType enumValue)
        {
          switch(enumValue)
          {
          case FunctionResponseType::NOT_SET:
            return {};
          case FunctionResponseType::ReportBatchItemFailures:
            return "ReportBatchItemFailures";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace FunctionResponseTypeMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
