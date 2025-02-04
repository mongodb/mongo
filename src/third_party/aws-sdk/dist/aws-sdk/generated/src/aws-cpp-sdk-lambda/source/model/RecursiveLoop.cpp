/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/RecursiveLoop.h>
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
      namespace RecursiveLoopMapper
      {

        static const int Allow_HASH = HashingUtils::HashString("Allow");
        static const int Terminate_HASH = HashingUtils::HashString("Terminate");


        RecursiveLoop GetRecursiveLoopForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Allow_HASH)
          {
            return RecursiveLoop::Allow;
          }
          else if (hashCode == Terminate_HASH)
          {
            return RecursiveLoop::Terminate;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<RecursiveLoop>(hashCode);
          }

          return RecursiveLoop::NOT_SET;
        }

        Aws::String GetNameForRecursiveLoop(RecursiveLoop enumValue)
        {
          switch(enumValue)
          {
          case RecursiveLoop::NOT_SET:
            return {};
          case RecursiveLoop::Allow:
            return "Allow";
          case RecursiveLoop::Terminate:
            return "Terminate";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace RecursiveLoopMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
