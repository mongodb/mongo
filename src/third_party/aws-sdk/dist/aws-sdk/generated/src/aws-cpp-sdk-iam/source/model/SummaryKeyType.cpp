/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/SummaryKeyType.h>
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
      namespace SummaryKeyTypeMapper
      {

        static const int Users_HASH = HashingUtils::HashString("Users");
        static const int UsersQuota_HASH = HashingUtils::HashString("UsersQuota");
        static const int Groups_HASH = HashingUtils::HashString("Groups");
        static const int GroupsQuota_HASH = HashingUtils::HashString("GroupsQuota");
        static const int ServerCertificates_HASH = HashingUtils::HashString("ServerCertificates");
        static const int ServerCertificatesQuota_HASH = HashingUtils::HashString("ServerCertificatesQuota");
        static const int UserPolicySizeQuota_HASH = HashingUtils::HashString("UserPolicySizeQuota");
        static const int GroupPolicySizeQuota_HASH = HashingUtils::HashString("GroupPolicySizeQuota");
        static const int GroupsPerUserQuota_HASH = HashingUtils::HashString("GroupsPerUserQuota");
        static const int SigningCertificatesPerUserQuota_HASH = HashingUtils::HashString("SigningCertificatesPerUserQuota");
        static const int AccessKeysPerUserQuota_HASH = HashingUtils::HashString("AccessKeysPerUserQuota");
        static const int MFADevices_HASH = HashingUtils::HashString("MFADevices");
        static const int MFADevicesInUse_HASH = HashingUtils::HashString("MFADevicesInUse");
        static const int AccountMFAEnabled_HASH = HashingUtils::HashString("AccountMFAEnabled");
        static const int AccountAccessKeysPresent_HASH = HashingUtils::HashString("AccountAccessKeysPresent");
        static const int AccountPasswordPresent_HASH = HashingUtils::HashString("AccountPasswordPresent");
        static const int AccountSigningCertificatesPresent_HASH = HashingUtils::HashString("AccountSigningCertificatesPresent");
        static const int AttachedPoliciesPerGroupQuota_HASH = HashingUtils::HashString("AttachedPoliciesPerGroupQuota");
        static const int AttachedPoliciesPerRoleQuota_HASH = HashingUtils::HashString("AttachedPoliciesPerRoleQuota");
        static const int AttachedPoliciesPerUserQuota_HASH = HashingUtils::HashString("AttachedPoliciesPerUserQuota");
        static const int Policies_HASH = HashingUtils::HashString("Policies");
        static const int PoliciesQuota_HASH = HashingUtils::HashString("PoliciesQuota");
        static const int PolicySizeQuota_HASH = HashingUtils::HashString("PolicySizeQuota");
        static const int PolicyVersionsInUse_HASH = HashingUtils::HashString("PolicyVersionsInUse");
        static const int PolicyVersionsInUseQuota_HASH = HashingUtils::HashString("PolicyVersionsInUseQuota");
        static const int VersionsPerPolicyQuota_HASH = HashingUtils::HashString("VersionsPerPolicyQuota");
        static const int GlobalEndpointTokenVersion_HASH = HashingUtils::HashString("GlobalEndpointTokenVersion");


        SummaryKeyType GetSummaryKeyTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Users_HASH)
          {
            return SummaryKeyType::Users;
          }
          else if (hashCode == UsersQuota_HASH)
          {
            return SummaryKeyType::UsersQuota;
          }
          else if (hashCode == Groups_HASH)
          {
            return SummaryKeyType::Groups;
          }
          else if (hashCode == GroupsQuota_HASH)
          {
            return SummaryKeyType::GroupsQuota;
          }
          else if (hashCode == ServerCertificates_HASH)
          {
            return SummaryKeyType::ServerCertificates;
          }
          else if (hashCode == ServerCertificatesQuota_HASH)
          {
            return SummaryKeyType::ServerCertificatesQuota;
          }
          else if (hashCode == UserPolicySizeQuota_HASH)
          {
            return SummaryKeyType::UserPolicySizeQuota;
          }
          else if (hashCode == GroupPolicySizeQuota_HASH)
          {
            return SummaryKeyType::GroupPolicySizeQuota;
          }
          else if (hashCode == GroupsPerUserQuota_HASH)
          {
            return SummaryKeyType::GroupsPerUserQuota;
          }
          else if (hashCode == SigningCertificatesPerUserQuota_HASH)
          {
            return SummaryKeyType::SigningCertificatesPerUserQuota;
          }
          else if (hashCode == AccessKeysPerUserQuota_HASH)
          {
            return SummaryKeyType::AccessKeysPerUserQuota;
          }
          else if (hashCode == MFADevices_HASH)
          {
            return SummaryKeyType::MFADevices;
          }
          else if (hashCode == MFADevicesInUse_HASH)
          {
            return SummaryKeyType::MFADevicesInUse;
          }
          else if (hashCode == AccountMFAEnabled_HASH)
          {
            return SummaryKeyType::AccountMFAEnabled;
          }
          else if (hashCode == AccountAccessKeysPresent_HASH)
          {
            return SummaryKeyType::AccountAccessKeysPresent;
          }
          else if (hashCode == AccountPasswordPresent_HASH)
          {
            return SummaryKeyType::AccountPasswordPresent;
          }
          else if (hashCode == AccountSigningCertificatesPresent_HASH)
          {
            return SummaryKeyType::AccountSigningCertificatesPresent;
          }
          else if (hashCode == AttachedPoliciesPerGroupQuota_HASH)
          {
            return SummaryKeyType::AttachedPoliciesPerGroupQuota;
          }
          else if (hashCode == AttachedPoliciesPerRoleQuota_HASH)
          {
            return SummaryKeyType::AttachedPoliciesPerRoleQuota;
          }
          else if (hashCode == AttachedPoliciesPerUserQuota_HASH)
          {
            return SummaryKeyType::AttachedPoliciesPerUserQuota;
          }
          else if (hashCode == Policies_HASH)
          {
            return SummaryKeyType::Policies;
          }
          else if (hashCode == PoliciesQuota_HASH)
          {
            return SummaryKeyType::PoliciesQuota;
          }
          else if (hashCode == PolicySizeQuota_HASH)
          {
            return SummaryKeyType::PolicySizeQuota;
          }
          else if (hashCode == PolicyVersionsInUse_HASH)
          {
            return SummaryKeyType::PolicyVersionsInUse;
          }
          else if (hashCode == PolicyVersionsInUseQuota_HASH)
          {
            return SummaryKeyType::PolicyVersionsInUseQuota;
          }
          else if (hashCode == VersionsPerPolicyQuota_HASH)
          {
            return SummaryKeyType::VersionsPerPolicyQuota;
          }
          else if (hashCode == GlobalEndpointTokenVersion_HASH)
          {
            return SummaryKeyType::GlobalEndpointTokenVersion;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<SummaryKeyType>(hashCode);
          }

          return SummaryKeyType::NOT_SET;
        }

        Aws::String GetNameForSummaryKeyType(SummaryKeyType enumValue)
        {
          switch(enumValue)
          {
          case SummaryKeyType::NOT_SET:
            return {};
          case SummaryKeyType::Users:
            return "Users";
          case SummaryKeyType::UsersQuota:
            return "UsersQuota";
          case SummaryKeyType::Groups:
            return "Groups";
          case SummaryKeyType::GroupsQuota:
            return "GroupsQuota";
          case SummaryKeyType::ServerCertificates:
            return "ServerCertificates";
          case SummaryKeyType::ServerCertificatesQuota:
            return "ServerCertificatesQuota";
          case SummaryKeyType::UserPolicySizeQuota:
            return "UserPolicySizeQuota";
          case SummaryKeyType::GroupPolicySizeQuota:
            return "GroupPolicySizeQuota";
          case SummaryKeyType::GroupsPerUserQuota:
            return "GroupsPerUserQuota";
          case SummaryKeyType::SigningCertificatesPerUserQuota:
            return "SigningCertificatesPerUserQuota";
          case SummaryKeyType::AccessKeysPerUserQuota:
            return "AccessKeysPerUserQuota";
          case SummaryKeyType::MFADevices:
            return "MFADevices";
          case SummaryKeyType::MFADevicesInUse:
            return "MFADevicesInUse";
          case SummaryKeyType::AccountMFAEnabled:
            return "AccountMFAEnabled";
          case SummaryKeyType::AccountAccessKeysPresent:
            return "AccountAccessKeysPresent";
          case SummaryKeyType::AccountPasswordPresent:
            return "AccountPasswordPresent";
          case SummaryKeyType::AccountSigningCertificatesPresent:
            return "AccountSigningCertificatesPresent";
          case SummaryKeyType::AttachedPoliciesPerGroupQuota:
            return "AttachedPoliciesPerGroupQuota";
          case SummaryKeyType::AttachedPoliciesPerRoleQuota:
            return "AttachedPoliciesPerRoleQuota";
          case SummaryKeyType::AttachedPoliciesPerUserQuota:
            return "AttachedPoliciesPerUserQuota";
          case SummaryKeyType::Policies:
            return "Policies";
          case SummaryKeyType::PoliciesQuota:
            return "PoliciesQuota";
          case SummaryKeyType::PolicySizeQuota:
            return "PolicySizeQuota";
          case SummaryKeyType::PolicyVersionsInUse:
            return "PolicyVersionsInUse";
          case SummaryKeyType::PolicyVersionsInUseQuota:
            return "PolicyVersionsInUseQuota";
          case SummaryKeyType::VersionsPerPolicyQuota:
            return "VersionsPerPolicyQuota";
          case SummaryKeyType::GlobalEndpointTokenVersion:
            return "GlobalEndpointTokenVersion";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace SummaryKeyTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
