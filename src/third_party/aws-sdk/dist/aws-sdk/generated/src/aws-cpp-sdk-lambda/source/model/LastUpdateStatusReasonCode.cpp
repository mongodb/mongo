/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/LastUpdateStatusReasonCode.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace Lambda
  {
    namespace Model
    {
      namespace LastUpdateStatusReasonCodeMapper
      {

        static const int EniLimitExceeded_HASH = HashingUtils::HashString("EniLimitExceeded");
        static const int InsufficientRolePermissions_HASH = HashingUtils::HashString("InsufficientRolePermissions");
        static const int InvalidConfiguration_HASH = HashingUtils::HashString("InvalidConfiguration");
        static const int InternalError_HASH = HashingUtils::HashString("InternalError");
        static const int SubnetOutOfIPAddresses_HASH = HashingUtils::HashString("SubnetOutOfIPAddresses");
        static const int InvalidSubnet_HASH = HashingUtils::HashString("InvalidSubnet");
        static const int InvalidSecurityGroup_HASH = HashingUtils::HashString("InvalidSecurityGroup");
        static const int ImageDeleted_HASH = HashingUtils::HashString("ImageDeleted");
        static const int ImageAccessDenied_HASH = HashingUtils::HashString("ImageAccessDenied");
        static const int InvalidImage_HASH = HashingUtils::HashString("InvalidImage");
        static const int KMSKeyAccessDenied_HASH = HashingUtils::HashString("KMSKeyAccessDenied");
        static const int KMSKeyNotFound_HASH = HashingUtils::HashString("KMSKeyNotFound");
        static const int InvalidStateKMSKey_HASH = HashingUtils::HashString("InvalidStateKMSKey");
        static const int DisabledKMSKey_HASH = HashingUtils::HashString("DisabledKMSKey");
        static const int EFSIOError_HASH = HashingUtils::HashString("EFSIOError");
        static const int EFSMountConnectivityError_HASH = HashingUtils::HashString("EFSMountConnectivityError");
        static const int EFSMountFailure_HASH = HashingUtils::HashString("EFSMountFailure");
        static const int EFSMountTimeout_HASH = HashingUtils::HashString("EFSMountTimeout");
        static const int InvalidRuntime_HASH = HashingUtils::HashString("InvalidRuntime");
        static const int InvalidZipFileException_HASH = HashingUtils::HashString("InvalidZipFileException");
        static const int FunctionError_HASH = HashingUtils::HashString("FunctionError");


        LastUpdateStatusReasonCode GetLastUpdateStatusReasonCodeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == EniLimitExceeded_HASH)
          {
            return LastUpdateStatusReasonCode::EniLimitExceeded;
          }
          else if (hashCode == InsufficientRolePermissions_HASH)
          {
            return LastUpdateStatusReasonCode::InsufficientRolePermissions;
          }
          else if (hashCode == InvalidConfiguration_HASH)
          {
            return LastUpdateStatusReasonCode::InvalidConfiguration;
          }
          else if (hashCode == InternalError_HASH)
          {
            return LastUpdateStatusReasonCode::InternalError;
          }
          else if (hashCode == SubnetOutOfIPAddresses_HASH)
          {
            return LastUpdateStatusReasonCode::SubnetOutOfIPAddresses;
          }
          else if (hashCode == InvalidSubnet_HASH)
          {
            return LastUpdateStatusReasonCode::InvalidSubnet;
          }
          else if (hashCode == InvalidSecurityGroup_HASH)
          {
            return LastUpdateStatusReasonCode::InvalidSecurityGroup;
          }
          else if (hashCode == ImageDeleted_HASH)
          {
            return LastUpdateStatusReasonCode::ImageDeleted;
          }
          else if (hashCode == ImageAccessDenied_HASH)
          {
            return LastUpdateStatusReasonCode::ImageAccessDenied;
          }
          else if (hashCode == InvalidImage_HASH)
          {
            return LastUpdateStatusReasonCode::InvalidImage;
          }
          else if (hashCode == KMSKeyAccessDenied_HASH)
          {
            return LastUpdateStatusReasonCode::KMSKeyAccessDenied;
          }
          else if (hashCode == KMSKeyNotFound_HASH)
          {
            return LastUpdateStatusReasonCode::KMSKeyNotFound;
          }
          else if (hashCode == InvalidStateKMSKey_HASH)
          {
            return LastUpdateStatusReasonCode::InvalidStateKMSKey;
          }
          else if (hashCode == DisabledKMSKey_HASH)
          {
            return LastUpdateStatusReasonCode::DisabledKMSKey;
          }
          else if (hashCode == EFSIOError_HASH)
          {
            return LastUpdateStatusReasonCode::EFSIOError;
          }
          else if (hashCode == EFSMountConnectivityError_HASH)
          {
            return LastUpdateStatusReasonCode::EFSMountConnectivityError;
          }
          else if (hashCode == EFSMountFailure_HASH)
          {
            return LastUpdateStatusReasonCode::EFSMountFailure;
          }
          else if (hashCode == EFSMountTimeout_HASH)
          {
            return LastUpdateStatusReasonCode::EFSMountTimeout;
          }
          else if (hashCode == InvalidRuntime_HASH)
          {
            return LastUpdateStatusReasonCode::InvalidRuntime;
          }
          else if (hashCode == InvalidZipFileException_HASH)
          {
            return LastUpdateStatusReasonCode::InvalidZipFileException;
          }
          else if (hashCode == FunctionError_HASH)
          {
            return LastUpdateStatusReasonCode::FunctionError;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<LastUpdateStatusReasonCode>(hashCode);
          }

          return LastUpdateStatusReasonCode::NOT_SET;
        }

        Aws::String GetNameForLastUpdateStatusReasonCode(LastUpdateStatusReasonCode enumValue)
        {
          switch(enumValue)
          {
          case LastUpdateStatusReasonCode::NOT_SET:
            return {};
          case LastUpdateStatusReasonCode::EniLimitExceeded:
            return "EniLimitExceeded";
          case LastUpdateStatusReasonCode::InsufficientRolePermissions:
            return "InsufficientRolePermissions";
          case LastUpdateStatusReasonCode::InvalidConfiguration:
            return "InvalidConfiguration";
          case LastUpdateStatusReasonCode::InternalError:
            return "InternalError";
          case LastUpdateStatusReasonCode::SubnetOutOfIPAddresses:
            return "SubnetOutOfIPAddresses";
          case LastUpdateStatusReasonCode::InvalidSubnet:
            return "InvalidSubnet";
          case LastUpdateStatusReasonCode::InvalidSecurityGroup:
            return "InvalidSecurityGroup";
          case LastUpdateStatusReasonCode::ImageDeleted:
            return "ImageDeleted";
          case LastUpdateStatusReasonCode::ImageAccessDenied:
            return "ImageAccessDenied";
          case LastUpdateStatusReasonCode::InvalidImage:
            return "InvalidImage";
          case LastUpdateStatusReasonCode::KMSKeyAccessDenied:
            return "KMSKeyAccessDenied";
          case LastUpdateStatusReasonCode::KMSKeyNotFound:
            return "KMSKeyNotFound";
          case LastUpdateStatusReasonCode::InvalidStateKMSKey:
            return "InvalidStateKMSKey";
          case LastUpdateStatusReasonCode::DisabledKMSKey:
            return "DisabledKMSKey";
          case LastUpdateStatusReasonCode::EFSIOError:
            return "EFSIOError";
          case LastUpdateStatusReasonCode::EFSMountConnectivityError:
            return "EFSMountConnectivityError";
          case LastUpdateStatusReasonCode::EFSMountFailure:
            return "EFSMountFailure";
          case LastUpdateStatusReasonCode::EFSMountTimeout:
            return "EFSMountTimeout";
          case LastUpdateStatusReasonCode::InvalidRuntime:
            return "InvalidRuntime";
          case LastUpdateStatusReasonCode::InvalidZipFileException:
            return "InvalidZipFileException";
          case LastUpdateStatusReasonCode::FunctionError:
            return "FunctionError";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace LastUpdateStatusReasonCodeMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
