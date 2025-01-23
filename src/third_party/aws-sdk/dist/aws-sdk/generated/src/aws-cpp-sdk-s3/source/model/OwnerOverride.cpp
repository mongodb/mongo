/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/OwnerOverride.h>
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
      namespace OwnerOverrideMapper
      {

        static const int Destination_HASH = HashingUtils::HashString("Destination");


        OwnerOverride GetOwnerOverrideForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Destination_HASH)
          {
            return OwnerOverride::Destination;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<OwnerOverride>(hashCode);
          }

          return OwnerOverride::NOT_SET;
        }

        Aws::String GetNameForOwnerOverride(OwnerOverride enumValue)
        {
          switch(enumValue)
          {
          case OwnerOverride::NOT_SET:
            return {};
          case OwnerOverride::Destination:
            return "Destination";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace OwnerOverrideMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
