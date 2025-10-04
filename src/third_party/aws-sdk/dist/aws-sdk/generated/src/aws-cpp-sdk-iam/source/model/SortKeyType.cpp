/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/SortKeyType.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace IAM
  {
    namespace Model
    {
      namespace SortKeyTypeMapper
      {

        static const int SERVICE_NAMESPACE_ASCENDING_HASH = HashingUtils::HashString("SERVICE_NAMESPACE_ASCENDING");
        static const int SERVICE_NAMESPACE_DESCENDING_HASH = HashingUtils::HashString("SERVICE_NAMESPACE_DESCENDING");
        static const int LAST_AUTHENTICATED_TIME_ASCENDING_HASH = HashingUtils::HashString("LAST_AUTHENTICATED_TIME_ASCENDING");
        static const int LAST_AUTHENTICATED_TIME_DESCENDING_HASH = HashingUtils::HashString("LAST_AUTHENTICATED_TIME_DESCENDING");


        SortKeyType GetSortKeyTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == SERVICE_NAMESPACE_ASCENDING_HASH)
          {
            return SortKeyType::SERVICE_NAMESPACE_ASCENDING;
          }
          else if (hashCode == SERVICE_NAMESPACE_DESCENDING_HASH)
          {
            return SortKeyType::SERVICE_NAMESPACE_DESCENDING;
          }
          else if (hashCode == LAST_AUTHENTICATED_TIME_ASCENDING_HASH)
          {
            return SortKeyType::LAST_AUTHENTICATED_TIME_ASCENDING;
          }
          else if (hashCode == LAST_AUTHENTICATED_TIME_DESCENDING_HASH)
          {
            return SortKeyType::LAST_AUTHENTICATED_TIME_DESCENDING;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<SortKeyType>(hashCode);
          }

          return SortKeyType::NOT_SET;
        }

        Aws::String GetNameForSortKeyType(SortKeyType enumValue)
        {
          switch(enumValue)
          {
          case SortKeyType::NOT_SET:
            return {};
          case SortKeyType::SERVICE_NAMESPACE_ASCENDING:
            return "SERVICE_NAMESPACE_ASCENDING";
          case SortKeyType::SERVICE_NAMESPACE_DESCENDING:
            return "SERVICE_NAMESPACE_DESCENDING";
          case SortKeyType::LAST_AUTHENTICATED_TIME_ASCENDING:
            return "LAST_AUTHENTICATED_TIME_ASCENDING";
          case SortKeyType::LAST_AUTHENTICATED_TIME_DESCENDING:
            return "LAST_AUTHENTICATED_TIME_DESCENDING";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace SortKeyTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
