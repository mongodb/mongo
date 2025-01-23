/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/StorageClassAnalysisSchemaVersion.h>
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
      namespace StorageClassAnalysisSchemaVersionMapper
      {

        static const int V_1_HASH = HashingUtils::HashString("V_1");


        StorageClassAnalysisSchemaVersion GetStorageClassAnalysisSchemaVersionForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == V_1_HASH)
          {
            return StorageClassAnalysisSchemaVersion::V_1;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<StorageClassAnalysisSchemaVersion>(hashCode);
          }

          return StorageClassAnalysisSchemaVersion::NOT_SET;
        }

        Aws::String GetNameForStorageClassAnalysisSchemaVersion(StorageClassAnalysisSchemaVersion enumValue)
        {
          switch(enumValue)
          {
          case StorageClassAnalysisSchemaVersion::NOT_SET:
            return {};
          case StorageClassAnalysisSchemaVersion::V_1:
            return "V_1";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace StorageClassAnalysisSchemaVersionMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
