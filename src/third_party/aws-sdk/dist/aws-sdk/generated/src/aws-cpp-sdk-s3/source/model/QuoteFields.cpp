/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/QuoteFields.h>
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
      namespace QuoteFieldsMapper
      {

        static const int ALWAYS_HASH = HashingUtils::HashString("ALWAYS");
        static const int ASNEEDED_HASH = HashingUtils::HashString("ASNEEDED");


        QuoteFields GetQuoteFieldsForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == ALWAYS_HASH)
          {
            return QuoteFields::ALWAYS;
          }
          else if (hashCode == ASNEEDED_HASH)
          {
            return QuoteFields::ASNEEDED;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<QuoteFields>(hashCode);
          }

          return QuoteFields::NOT_SET;
        }

        Aws::String GetNameForQuoteFields(QuoteFields enumValue)
        {
          switch(enumValue)
          {
          case QuoteFields::NOT_SET:
            return {};
          case QuoteFields::ALWAYS:
            return "ALWAYS";
          case QuoteFields::ASNEEDED:
            return "ASNEEDED";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace QuoteFieldsMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
