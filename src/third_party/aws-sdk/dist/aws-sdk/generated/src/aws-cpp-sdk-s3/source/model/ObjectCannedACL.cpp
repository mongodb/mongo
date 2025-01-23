/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ObjectCannedACL.h>
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
      namespace ObjectCannedACLMapper
      {

        static const int private__HASH = HashingUtils::HashString("private");
        static const int public_read_HASH = HashingUtils::HashString("public-read");
        static const int public_read_write_HASH = HashingUtils::HashString("public-read-write");
        static const int authenticated_read_HASH = HashingUtils::HashString("authenticated-read");
        static const int aws_exec_read_HASH = HashingUtils::HashString("aws-exec-read");
        static const int bucket_owner_read_HASH = HashingUtils::HashString("bucket-owner-read");
        static const int bucket_owner_full_control_HASH = HashingUtils::HashString("bucket-owner-full-control");


        ObjectCannedACL GetObjectCannedACLForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == private__HASH)
          {
            return ObjectCannedACL::private_;
          }
          else if (hashCode == public_read_HASH)
          {
            return ObjectCannedACL::public_read;
          }
          else if (hashCode == public_read_write_HASH)
          {
            return ObjectCannedACL::public_read_write;
          }
          else if (hashCode == authenticated_read_HASH)
          {
            return ObjectCannedACL::authenticated_read;
          }
          else if (hashCode == aws_exec_read_HASH)
          {
            return ObjectCannedACL::aws_exec_read;
          }
          else if (hashCode == bucket_owner_read_HASH)
          {
            return ObjectCannedACL::bucket_owner_read;
          }
          else if (hashCode == bucket_owner_full_control_HASH)
          {
            return ObjectCannedACL::bucket_owner_full_control;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ObjectCannedACL>(hashCode);
          }

          return ObjectCannedACL::NOT_SET;
        }

        Aws::String GetNameForObjectCannedACL(ObjectCannedACL enumValue)
        {
          switch(enumValue)
          {
          case ObjectCannedACL::NOT_SET:
            return {};
          case ObjectCannedACL::private_:
            return "private";
          case ObjectCannedACL::public_read:
            return "public-read";
          case ObjectCannedACL::public_read_write:
            return "public-read-write";
          case ObjectCannedACL::authenticated_read:
            return "authenticated-read";
          case ObjectCannedACL::aws_exec_read:
            return "aws-exec-read";
          case ObjectCannedACL::bucket_owner_read:
            return "bucket-owner-read";
          case ObjectCannedACL::bucket_owner_full_control:
            return "bucket-owner-full-control";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ObjectCannedACLMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
