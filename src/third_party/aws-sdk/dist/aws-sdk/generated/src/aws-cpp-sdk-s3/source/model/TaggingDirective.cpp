/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/TaggingDirective.h>
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
      namespace TaggingDirectiveMapper
      {

        static const int COPY_HASH = HashingUtils::HashString("COPY");
        static const int REPLACE_HASH = HashingUtils::HashString("REPLACE");


        TaggingDirective GetTaggingDirectiveForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == COPY_HASH)
          {
            return TaggingDirective::COPY;
          }
          else if (hashCode == REPLACE_HASH)
          {
            return TaggingDirective::REPLACE;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<TaggingDirective>(hashCode);
          }

          return TaggingDirective::NOT_SET;
        }

        Aws::String GetNameForTaggingDirective(TaggingDirective enumValue)
        {
          switch(enumValue)
          {
          case TaggingDirective::NOT_SET:
            return {};
          case TaggingDirective::COPY:
            return "COPY";
          case TaggingDirective::REPLACE:
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

      } // namespace TaggingDirectiveMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
