/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/Payer.h>
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
      namespace PayerMapper
      {

        static const int Requester_HASH = HashingUtils::HashString("Requester");
        static const int BucketOwner_HASH = HashingUtils::HashString("BucketOwner");


        Payer GetPayerForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Requester_HASH)
          {
            return Payer::Requester;
          }
          else if (hashCode == BucketOwner_HASH)
          {
            return Payer::BucketOwner;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<Payer>(hashCode);
          }

          return Payer::NOT_SET;
        }

        Aws::String GetNameForPayer(Payer enumValue)
        {
          switch(enumValue)
          {
          case Payer::NOT_SET:
            return {};
          case Payer::Requester:
            return "Requester";
          case Payer::BucketOwner:
            return "BucketOwner";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace PayerMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
