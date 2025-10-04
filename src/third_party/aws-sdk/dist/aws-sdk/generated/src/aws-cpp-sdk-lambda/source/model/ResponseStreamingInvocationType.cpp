/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/ResponseStreamingInvocationType.h>
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
      namespace ResponseStreamingInvocationTypeMapper
      {

        static const int RequestResponse_HASH = HashingUtils::HashString("RequestResponse");
        static const int DryRun_HASH = HashingUtils::HashString("DryRun");


        ResponseStreamingInvocationType GetResponseStreamingInvocationTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == RequestResponse_HASH)
          {
            return ResponseStreamingInvocationType::RequestResponse;
          }
          else if (hashCode == DryRun_HASH)
          {
            return ResponseStreamingInvocationType::DryRun;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ResponseStreamingInvocationType>(hashCode);
          }

          return ResponseStreamingInvocationType::NOT_SET;
        }

        Aws::String GetNameForResponseStreamingInvocationType(ResponseStreamingInvocationType enumValue)
        {
          switch(enumValue)
          {
          case ResponseStreamingInvocationType::NOT_SET:
            return {};
          case ResponseStreamingInvocationType::RequestResponse:
            return "RequestResponse";
          case ResponseStreamingInvocationType::DryRun:
            return "DryRun";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ResponseStreamingInvocationTypeMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
