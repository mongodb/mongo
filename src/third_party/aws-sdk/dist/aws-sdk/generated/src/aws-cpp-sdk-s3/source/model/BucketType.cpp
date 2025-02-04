/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/BucketType.h>
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
      namespace BucketTypeMapper
      {

        static const int Directory_HASH = HashingUtils::HashString("Directory");


        BucketType GetBucketTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Directory_HASH)
          {
            return BucketType::Directory;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<BucketType>(hashCode);
          }

          return BucketType::NOT_SET;
        }

        Aws::String GetNameForBucketType(BucketType enumValue)
        {
          switch(enumValue)
          {
          case BucketType::NOT_SET:
            return {};
          case BucketType::Directory:
            return "Directory";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace BucketTypeMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
