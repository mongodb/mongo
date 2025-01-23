/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/MetadataDirective.h>
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
      namespace MetadataDirectiveMapper
      {

        static const int COPY_HASH = HashingUtils::HashString("COPY");
        static const int REPLACE_HASH = HashingUtils::HashString("REPLACE");


        MetadataDirective GetMetadataDirectiveForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == COPY_HASH)
          {
            return MetadataDirective::COPY;
          }
          else if (hashCode == REPLACE_HASH)
          {
            return MetadataDirective::REPLACE;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<MetadataDirective>(hashCode);
          }

          return MetadataDirective::NOT_SET;
        }

        Aws::String GetNameForMetadataDirective(MetadataDirective enumValue)
        {
          switch(enumValue)
          {
          case MetadataDirective::NOT_SET:
            return {};
          case MetadataDirective::COPY:
            return "COPY";
          case MetadataDirective::REPLACE:
            return "REPLACE";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace MetadataDirectiveMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
