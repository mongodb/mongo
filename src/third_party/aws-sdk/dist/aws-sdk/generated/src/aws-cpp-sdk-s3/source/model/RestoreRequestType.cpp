/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/RestoreRequestType.h>
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
      namespace RestoreRequestTypeMapper
      {

        static const int SELECT_HASH = HashingUtils::HashString("SELECT");


        RestoreRequestType GetRestoreRequestTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == SELECT_HASH)
          {
            return RestoreRequestType::SELECT;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<RestoreRequestType>(hashCode);
          }

          return RestoreRequestType::NOT_SET;
        }

        Aws::String GetNameForRestoreRequestType(RestoreRequestType enumValue)
        {
          switch(enumValue)
          {
          case RestoreRequestType::NOT_SET:
            return {};
          case RestoreRequestType::SELECT:
            return "SELECT";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace RestoreRequestTypeMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
