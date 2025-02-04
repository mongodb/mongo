/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/SseKmsEncryptedObjectsStatus.h>
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
      namespace SseKmsEncryptedObjectsStatusMapper
      {

        static const int Enabled_HASH = HashingUtils::HashString("Enabled");
        static const int Disabled_HASH = HashingUtils::HashString("Disabled");


        SseKmsEncryptedObjectsStatus GetSseKmsEncryptedObjectsStatusForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Enabled_HASH)
          {
            return SseKmsEncryptedObjectsStatus::Enabled;
          }
          else if (hashCode == Disabled_HASH)
          {
            return SseKmsEncryptedObjectsStatus::Disabled;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<SseKmsEncryptedObjectsStatus>(hashCode);
          }

          return SseKmsEncryptedObjectsStatus::NOT_SET;
        }

        Aws::String GetNameForSseKmsEncryptedObjectsStatus(SseKmsEncryptedObjectsStatus enumValue)
        {
          switch(enumValue)
          {
          case SseKmsEncryptedObjectsStatus::NOT_SET:
            return {};
          case SseKmsEncryptedObjectsStatus::Enabled:
            return "Enabled";
          case SseKmsEncryptedObjectsStatus::Disabled:
            return "Disabled";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace SseKmsEncryptedObjectsStatusMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
