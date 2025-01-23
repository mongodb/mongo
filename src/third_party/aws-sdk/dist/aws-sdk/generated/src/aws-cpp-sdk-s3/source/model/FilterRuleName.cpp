/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/FilterRuleName.h>
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
      namespace FilterRuleNameMapper
      {

        static const int prefix_HASH = HashingUtils::HashString("prefix");
        static const int suffix_HASH = HashingUtils::HashString("suffix");


        FilterRuleName GetFilterRuleNameForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == prefix_HASH)
          {
            return FilterRuleName::prefix;
          }
          else if (hashCode == suffix_HASH)
          {
            return FilterRuleName::suffix;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<FilterRuleName>(hashCode);
          }

          return FilterRuleName::NOT_SET;
        }

        Aws::String GetNameForFilterRuleName(FilterRuleName enumValue)
        {
          switch(enumValue)
          {
          case FilterRuleName::NOT_SET:
            return {};
          case FilterRuleName::prefix:
            return "prefix";
          case FilterRuleName::suffix:
            return "suffix";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace FilterRuleNameMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
