/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/RequestCharged.h>
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
      namespace RequestChargedMapper
      {

        static const int requester_HASH = HashingUtils::HashString("requester");


        RequestCharged GetRequestChargedForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == requester_HASH)
          {
            return RequestCharged::requester;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<RequestCharged>(hashCode);
          }

          return RequestCharged::NOT_SET;
        }

        Aws::String GetNameForRequestCharged(RequestCharged enumValue)
        {
          switch(enumValue)
          {
          case RequestCharged::NOT_SET:
            return {};
          case RequestCharged::requester:
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

      } // namespace RequestChargedMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
