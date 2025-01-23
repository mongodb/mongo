/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/TransitionDefaultMinimumObjectSize.h>
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
      namespace TransitionDefaultMinimumObjectSizeMapper
      {

        static const int varies_by_storage_class_HASH = HashingUtils::HashString("varies_by_storage_class");
        static const int all_storage_classes_128K_HASH = HashingUtils::HashString("all_storage_classes_128K");


        TransitionDefaultMinimumObjectSize GetTransitionDefaultMinimumObjectSizeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == varies_by_storage_class_HASH)
          {
            return TransitionDefaultMinimumObjectSize::varies_by_storage_class;
          }
          else if (hashCode == all_storage_classes_128K_HASH)
          {
            return TransitionDefaultMinimumObjectSize::all_storage_classes_128K;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<TransitionDefaultMinimumObjectSize>(hashCode);
          }

          return TransitionDefaultMinimumObjectSize::NOT_SET;
        }

        Aws::String GetNameForTransitionDefaultMinimumObjectSize(TransitionDefaultMinimumObjectSize enumValue)
        {
          switch(enumValue)
          {
          case TransitionDefaultMinimumObjectSize::NOT_SET:
            return {};
          case TransitionDefaultMinimumObjectSize::varies_by_storage_class:
            return "varies_by_storage_class";
          case TransitionDefaultMinimumObjectSize::all_storage_classes_128K:
            return "all_storage_classes_128K";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace TransitionDefaultMinimumObjectSizeMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
