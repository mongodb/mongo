/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/FullDocument.h>
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
      namespace FullDocumentMapper
      {

        static const int UpdateLookup_HASH = HashingUtils::HashString("UpdateLookup");
        static const int Default_HASH = HashingUtils::HashString("Default");


        FullDocument GetFullDocumentForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == UpdateLookup_HASH)
          {
            return FullDocument::UpdateLookup;
          }
          else if (hashCode == Default_HASH)
          {
            return FullDocument::Default;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<FullDocument>(hashCode);
          }

          return FullDocument::NOT_SET;
        }

        Aws::String GetNameForFullDocument(FullDocument enumValue)
        {
          switch(enumValue)
          {
          case FullDocument::NOT_SET:
            return {};
          case FullDocument::UpdateLookup:
            return "UpdateLookup";
          case FullDocument::Default:
            return "Default";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace FullDocumentMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
