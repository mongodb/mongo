/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/OptionalObjectAttributes.h>
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
      namespace OptionalObjectAttributesMapper
      {

        static const int RestoreStatus_HASH = HashingUtils::HashString("RestoreStatus");


        OptionalObjectAttributes GetOptionalObjectAttributesForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == RestoreStatus_HASH)
          {
            return OptionalObjectAttributes::RestoreStatus;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<OptionalObjectAttributes>(hashCode);
          }

          return OptionalObjectAttributes::NOT_SET;
        }

        Aws::String GetNameForOptionalObjectAttributes(OptionalObjectAttributes enumValue)
        {
          switch(enumValue)
          {
          case OptionalObjectAttributes::NOT_SET:
            return {};
          case OptionalObjectAttributes::RestoreStatus:
            return "RestoreStatus";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace OptionalObjectAttributesMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
