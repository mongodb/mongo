/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/UpdateRuntimeOn.h>
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
      namespace UpdateRuntimeOnMapper
      {

        static const int Auto_HASH = HashingUtils::HashString("Auto");
        static const int Manual_HASH = HashingUtils::HashString("Manual");
        static const int FunctionUpdate_HASH = HashingUtils::HashString("FunctionUpdate");


        UpdateRuntimeOn GetUpdateRuntimeOnForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Auto_HASH)
          {
            return UpdateRuntimeOn::Auto;
          }
          else if (hashCode == Manual_HASH)
          {
            return UpdateRuntimeOn::Manual;
          }
          else if (hashCode == FunctionUpdate_HASH)
          {
            return UpdateRuntimeOn::FunctionUpdate;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<UpdateRuntimeOn>(hashCode);
          }

          return UpdateRuntimeOn::NOT_SET;
        }

        Aws::String GetNameForUpdateRuntimeOn(UpdateRuntimeOn enumValue)
        {
          switch(enumValue)
          {
          case UpdateRuntimeOn::NOT_SET:
            return {};
          case UpdateRuntimeOn::Auto:
            return "Auto";
          case UpdateRuntimeOn::Manual:
            return "Manual";
          case UpdateRuntimeOn::FunctionUpdate:
            return "FunctionUpdate";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace UpdateRuntimeOnMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
