/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/InvokeMode.h>
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
      namespace InvokeModeMapper
      {

        static const int BUFFERED_HASH = HashingUtils::HashString("BUFFERED");
        static const int RESPONSE_STREAM_HASH = HashingUtils::HashString("RESPONSE_STREAM");


        InvokeMode GetInvokeModeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == BUFFERED_HASH)
          {
            return InvokeMode::BUFFERED;
          }
          else if (hashCode == RESPONSE_STREAM_HASH)
          {
            return InvokeMode::RESPONSE_STREAM;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<InvokeMode>(hashCode);
          }

          return InvokeMode::NOT_SET;
        }

        Aws::String GetNameForInvokeMode(InvokeMode enumValue)
        {
          switch(enumValue)
          {
          case InvokeMode::NOT_SET:
            return {};
          case InvokeMode::BUFFERED:
            return "BUFFERED";
          case InvokeMode::RESPONSE_STREAM:
            return "RESPONSE_STREAM";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace InvokeModeMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
