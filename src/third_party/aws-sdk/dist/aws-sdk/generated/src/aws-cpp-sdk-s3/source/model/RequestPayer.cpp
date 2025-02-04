/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/RequestPayer.h>
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
      namespace RequestPayerMapper
      {

        static const int requester_HASH = HashingUtils::HashString("requester");


        RequestPayer GetRequestPayerForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == requester_HASH)
          {
            return RequestPayer::requester;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<RequestPayer>(hashCode);
          }

          return RequestPayer::NOT_SET;
        }

        Aws::String GetNameForRequestPayer(RequestPayer enumValue)
        {
          switch(enumValue)
          {
          case RequestPayer::NOT_SET:
            return {};
          case RequestPayer::requester:
            return "requester";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace RequestPayerMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
