/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ObjectLockEnabled.h>
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
      namespace ObjectLockEnabledMapper
      {

        static const int Enabled_HASH = HashingUtils::HashString("Enabled");


        ObjectLockEnabled GetObjectLockEnabledForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Enabled_HASH)
          {
            return ObjectLockEnabled::Enabled;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ObjectLockEnabled>(hashCode);
          }

          return ObjectLockEnabled::NOT_SET;
        }

        Aws::String GetNameForObjectLockEnabled(ObjectLockEnabled enumValue)
        {
          switch(enumValue)
          {
          case ObjectLockEnabled::NOT_SET:
            return {};
          case ObjectLockEnabled::Enabled:
            return "Enabled";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ObjectLockEnabledMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
