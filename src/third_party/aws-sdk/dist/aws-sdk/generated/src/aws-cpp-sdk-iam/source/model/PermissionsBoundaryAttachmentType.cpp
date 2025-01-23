/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/PermissionsBoundaryAttachmentType.h>
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
      namespace PermissionsBoundaryAttachmentTypeMapper
      {

        static const int PermissionsBoundaryPolicy_HASH = HashingUtils::HashString("PermissionsBoundaryPolicy");


        PermissionsBoundaryAttachmentType GetPermissionsBoundaryAttachmentTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == PermissionsBoundaryPolicy_HASH)
          {
            return PermissionsBoundaryAttachmentType::PermissionsBoundaryPolicy;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<PermissionsBoundaryAttachmentType>(hashCode);
          }

          return PermissionsBoundaryAttachmentType::NOT_SET;
        }

        Aws::String GetNameForPermissionsBoundaryAttachmentType(PermissionsBoundaryAttachmentType enumValue)
        {
          switch(enumValue)
          {
          case PermissionsBoundaryAttachmentType::NOT_SET:
            return {};
          case PermissionsBoundaryAttachmentType::PermissionsBoundaryPolicy:
            return "PermissionsBoundaryPolicy";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace PermissionsBoundaryAttachmentTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
