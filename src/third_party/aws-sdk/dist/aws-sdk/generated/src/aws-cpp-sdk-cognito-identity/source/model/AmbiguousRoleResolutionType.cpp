/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/AmbiguousRoleResolutionType.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace CognitoIdentity
  {
    namespace Model
    {
      namespace AmbiguousRoleResolutionTypeMapper
      {

        static const int AuthenticatedRole_HASH = HashingUtils::HashString("AuthenticatedRole");
        static const int Deny_HASH = HashingUtils::HashString("Deny");


        AmbiguousRoleResolutionType GetAmbiguousRoleResolutionTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == AuthenticatedRole_HASH)
          {
            return AmbiguousRoleResolutionType::AuthenticatedRole;
          }
          else if (hashCode == Deny_HASH)
          {
            return AmbiguousRoleResolutionType::Deny;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<AmbiguousRoleResolutionType>(hashCode);
          }

          return AmbiguousRoleResolutionType::NOT_SET;
        }

        Aws::String GetNameForAmbiguousRoleResolutionType(AmbiguousRoleResolutionType enumValue)
        {
          switch(enumValue)
          {
          case AmbiguousRoleResolutionType::NOT_SET:
            return {};
          case AmbiguousRoleResolutionType::AuthenticatedRole:
            return "AuthenticatedRole";
          case AmbiguousRoleResolutionType::Deny:
            return "Deny";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace AmbiguousRoleResolutionTypeMapper
    } // namespace Model
  } // namespace CognitoIdentity
} // namespace Aws
