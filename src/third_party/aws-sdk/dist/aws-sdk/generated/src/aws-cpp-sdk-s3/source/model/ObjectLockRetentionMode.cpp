/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ObjectLockRetentionMode.h>
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
      namespace ObjectLockRetentionModeMapper
      {

        static const int GOVERNANCE_HASH = HashingUtils::HashString("GOVERNANCE");
        static const int COMPLIANCE_HASH = HashingUtils::HashString("COMPLIANCE");


        ObjectLockRetentionMode GetObjectLockRetentionModeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == GOVERNANCE_HASH)
          {
            return ObjectLockRetentionMode::GOVERNANCE;
          }
          else if (hashCode == COMPLIANCE_HASH)
          {
            return ObjectLockRetentionMode::COMPLIANCE;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ObjectLockRetentionMode>(hashCode);
          }

          return ObjectLockRetentionMode::NOT_SET;
        }

        Aws::String GetNameForObjectLockRetentionMode(ObjectLockRetentionMode enumValue)
        {
          switch(enumValue)
          {
          case ObjectLockRetentionMode::NOT_SET:
            return {};
          case ObjectLockRetentionMode::GOVERNANCE:
            return "GOVERNANCE";
          case ObjectLockRetentionMode::COMPLIANCE:
            return "COMPLIANCE";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ObjectLockRetentionModeMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
