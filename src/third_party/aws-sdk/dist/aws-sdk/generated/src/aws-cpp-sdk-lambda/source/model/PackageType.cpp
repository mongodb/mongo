/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/PackageType.h>
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
      namespace PackageTypeMapper
      {

        static const int Zip_HASH = HashingUtils::HashString("Zip");
        static const int Image_HASH = HashingUtils::HashString("Image");


        PackageType GetPackageTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Zip_HASH)
          {
            return PackageType::Zip;
          }
          else if (hashCode == Image_HASH)
          {
            return PackageType::Image;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<PackageType>(hashCode);
          }

          return PackageType::NOT_SET;
        }

        Aws::String GetNameForPackageType(PackageType enumValue)
        {
          switch(enumValue)
          {
          case PackageType::NOT_SET:
            return {};
          case PackageType::Zip:
            return "Zip";
          case PackageType::Image:
            return "Image";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace PackageTypeMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
