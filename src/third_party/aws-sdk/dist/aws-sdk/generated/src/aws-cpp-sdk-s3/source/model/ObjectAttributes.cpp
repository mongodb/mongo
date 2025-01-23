/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ObjectAttributes.h>
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
      namespace ObjectAttributesMapper
      {

        static const int ETag_HASH = HashingUtils::HashString("ETag");
        static const int Checksum_HASH = HashingUtils::HashString("Checksum");
        static const int ObjectParts_HASH = HashingUtils::HashString("ObjectParts");
        static const int StorageClass_HASH = HashingUtils::HashString("StorageClass");
        static const int ObjectSize_HASH = HashingUtils::HashString("ObjectSize");


        ObjectAttributes GetObjectAttributesForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == ETag_HASH)
          {
            return ObjectAttributes::ETag;
          }
          else if (hashCode == Checksum_HASH)
          {
            return ObjectAttributes::Checksum;
          }
          else if (hashCode == ObjectParts_HASH)
          {
            return ObjectAttributes::ObjectParts;
          }
          else if (hashCode == StorageClass_HASH)
          {
            return ObjectAttributes::StorageClass;
          }
          else if (hashCode == ObjectSize_HASH)
          {
            return ObjectAttributes::ObjectSize;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ObjectAttributes>(hashCode);
          }

          return ObjectAttributes::NOT_SET;
        }

        Aws::String GetNameForObjectAttributes(ObjectAttributes enumValue)
        {
          switch(enumValue)
          {
          case ObjectAttributes::NOT_SET:
            return {};
          case ObjectAttributes::ETag:
            return "ETag";
          case ObjectAttributes::Checksum:
            return "Checksum";
          case ObjectAttributes::ObjectParts:
            return "ObjectParts";
          case ObjectAttributes::StorageClass:
            return "StorageClass";
          case ObjectAttributes::ObjectSize:
            return "ObjectSize";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ObjectAttributesMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
