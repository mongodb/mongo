/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/FeatureType.h>
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
      namespace FeatureTypeMapper
      {

        static const int RootCredentialsManagement_HASH = HashingUtils::HashString("RootCredentialsManagement");
        static const int RootSessions_HASH = HashingUtils::HashString("RootSessions");


        FeatureType GetFeatureTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == RootCredentialsManagement_HASH)
          {
            return FeatureType::RootCredentialsManagement;
          }
          else if (hashCode == RootSessions_HASH)
          {
            return FeatureType::RootSessions;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<FeatureType>(hashCode);
          }

          return FeatureType::NOT_SET;
        }

        Aws::String GetNameForFeatureType(FeatureType enumValue)
        {
          switch(enumValue)
          {
          case FeatureType::NOT_SET:
            return {};
          case FeatureType::RootCredentialsManagement:
            return "RootCredentialsManagement";
          case FeatureType::RootSessions:
            return "RootSessions";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace FeatureTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
