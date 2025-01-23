/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/ObjectVersionStorageClass.h>
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
      namespace ObjectVersionStorageClassMapper
      {

        static const int STANDARD_HASH = HashingUtils::HashString("STANDARD");


        ObjectVersionStorageClass GetObjectVersionStorageClassForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == STANDARD_HASH)
          {
            return ObjectVersionStorageClass::STANDARD;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ObjectVersionStorageClass>(hashCode);
          }

          return ObjectVersionStorageClass::NOT_SET;
        }

        Aws::String GetNameForObjectVersionStorageClass(ObjectVersionStorageClass enumValue)
        {
          switch(enumValue)
          {
          case ObjectVersionStorageClass::NOT_SET:
            return {};
          case ObjectVersionStorageClass::STANDARD:
            return "STANDARD";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ObjectVersionStorageClassMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
