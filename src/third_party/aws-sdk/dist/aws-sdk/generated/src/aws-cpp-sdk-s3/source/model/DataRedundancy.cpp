/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/DataRedundancy.h>
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
      namespace DataRedundancyMapper
      {

        static const int SingleAvailabilityZone_HASH = HashingUtils::HashString("SingleAvailabilityZone");
        static const int SingleLocalZone_HASH = HashingUtils::HashString("SingleLocalZone");


        DataRedundancy GetDataRedundancyForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == SingleAvailabilityZone_HASH)
          {
            return DataRedundancy::SingleAvailabilityZone;
          }
          else if (hashCode == SingleLocalZone_HASH)
          {
            return DataRedundancy::SingleLocalZone;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<DataRedundancy>(hashCode);
          }

          return DataRedundancy::NOT_SET;
        }

        Aws::String GetNameForDataRedundancy(DataRedundancy enumValue)
        {
          switch(enumValue)
          {
          case DataRedundancy::NOT_SET:
            return {};
          case DataRedundancy::SingleAvailabilityZone:
            return "SingleAvailabilityZone";
          case DataRedundancy::SingleLocalZone:
            return "SingleLocalZone";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace DataRedundancyMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
