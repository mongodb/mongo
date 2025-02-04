/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ObjectStorageClass.h>
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
      namespace ObjectStorageClassMapper
      {

        static const int STANDARD_HASH = HashingUtils::HashString("STANDARD");
        static const int REDUCED_REDUNDANCY_HASH = HashingUtils::HashString("REDUCED_REDUNDANCY");
        static const int GLACIER_HASH = HashingUtils::HashString("GLACIER");
        static const int STANDARD_IA_HASH = HashingUtils::HashString("STANDARD_IA");
        static const int ONEZONE_IA_HASH = HashingUtils::HashString("ONEZONE_IA");
        static const int INTELLIGENT_TIERING_HASH = HashingUtils::HashString("INTELLIGENT_TIERING");
        static const int DEEP_ARCHIVE_HASH = HashingUtils::HashString("DEEP_ARCHIVE");
        static const int OUTPOSTS_HASH = HashingUtils::HashString("OUTPOSTS");
        static const int GLACIER_IR_HASH = HashingUtils::HashString("GLACIER_IR");
        static const int SNOW_HASH = HashingUtils::HashString("SNOW");
        static const int EXPRESS_ONEZONE_HASH = HashingUtils::HashString("EXPRESS_ONEZONE");


        ObjectStorageClass GetObjectStorageClassForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == STANDARD_HASH)
          {
            return ObjectStorageClass::STANDARD;
          }
          else if (hashCode == REDUCED_REDUNDANCY_HASH)
          {
            return ObjectStorageClass::REDUCED_REDUNDANCY;
          }
          else if (hashCode == GLACIER_HASH)
          {
            return ObjectStorageClass::GLACIER;
          }
          else if (hashCode == STANDARD_IA_HASH)
          {
            return ObjectStorageClass::STANDARD_IA;
          }
          else if (hashCode == ONEZONE_IA_HASH)
          {
            return ObjectStorageClass::ONEZONE_IA;
          }
          else if (hashCode == INTELLIGENT_TIERING_HASH)
          {
            return ObjectStorageClass::INTELLIGENT_TIERING;
          }
          else if (hashCode == DEEP_ARCHIVE_HASH)
          {
            return ObjectStorageClass::DEEP_ARCHIVE;
          }
          else if (hashCode == OUTPOSTS_HASH)
          {
            return ObjectStorageClass::OUTPOSTS;
          }
          else if (hashCode == GLACIER_IR_HASH)
          {
            return ObjectStorageClass::GLACIER_IR;
          }
          else if (hashCode == SNOW_HASH)
          {
            return ObjectStorageClass::SNOW;
          }
          else if (hashCode == EXPRESS_ONEZONE_HASH)
          {
            return ObjectStorageClass::EXPRESS_ONEZONE;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ObjectStorageClass>(hashCode);
          }

          return ObjectStorageClass::NOT_SET;
        }

        Aws::String GetNameForObjectStorageClass(ObjectStorageClass enumValue)
        {
          switch(enumValue)
          {
          case ObjectStorageClass::NOT_SET:
            return {};
          case ObjectStorageClass::STANDARD:
            return "STANDARD";
          case ObjectStorageClass::REDUCED_REDUNDANCY:
            return "REDUCED_REDUNDANCY";
          case ObjectStorageClass::GLACIER:
            return "GLACIER";
          case ObjectStorageClass::STANDARD_IA:
            return "STANDARD_IA";
          case ObjectStorageClass::ONEZONE_IA:
            return "ONEZONE_IA";
          case ObjectStorageClass::INTELLIGENT_TIERING:
            return "INTELLIGENT_TIERING";
          case ObjectStorageClass::DEEP_ARCHIVE:
            return "DEEP_ARCHIVE";
          case ObjectStorageClass::OUTPOSTS:
            return "OUTPOSTS";
          case ObjectStorageClass::GLACIER_IR:
            return "GLACIER_IR";
          case ObjectStorageClass::SNOW:
            return "SNOW";
          case ObjectStorageClass::EXPRESS_ONEZONE:
            return "EXPRESS_ONEZONE";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ObjectStorageClassMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
