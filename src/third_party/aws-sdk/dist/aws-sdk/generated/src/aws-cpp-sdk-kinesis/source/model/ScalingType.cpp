/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/ScalingType.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace Kinesis
  {
    namespace Model
    {
      namespace ScalingTypeMapper
      {

        static const int UNIFORM_SCALING_HASH = HashingUtils::HashString("UNIFORM_SCALING");


        ScalingType GetScalingTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == UNIFORM_SCALING_HASH)
          {
            return ScalingType::UNIFORM_SCALING;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ScalingType>(hashCode);
          }

          return ScalingType::NOT_SET;
        }

        Aws::String GetNameForScalingType(ScalingType enumValue)
        {
          switch(enumValue)
          {
          case ScalingType::NOT_SET:
            return {};
          case ScalingType::UNIFORM_SCALING:
            return "UNIFORM_SCALING";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ScalingTypeMapper
    } // namespace Model
  } // namespace Kinesis
} // namespace Aws
