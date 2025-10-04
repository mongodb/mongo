/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/CodeSigningPolicy.h>
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
      namespace CodeSigningPolicyMapper
      {

        static const int Warn_HASH = HashingUtils::HashString("Warn");
        static const int Enforce_HASH = HashingUtils::HashString("Enforce");


        CodeSigningPolicy GetCodeSigningPolicyForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Warn_HASH)
          {
            return CodeSigningPolicy::Warn;
          }
          else if (hashCode == Enforce_HASH)
          {
            return CodeSigningPolicy::Enforce;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<CodeSigningPolicy>(hashCode);
          }

          return CodeSigningPolicy::NOT_SET;
        }

        Aws::String GetNameForCodeSigningPolicy(CodeSigningPolicy enumValue)
        {
          switch(enumValue)
          {
          case CodeSigningPolicy::NOT_SET:
            return {};
          case CodeSigningPolicy::Warn:
            return "Warn";
          case CodeSigningPolicy::Enforce:
            return "Enforce";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace CodeSigningPolicyMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
