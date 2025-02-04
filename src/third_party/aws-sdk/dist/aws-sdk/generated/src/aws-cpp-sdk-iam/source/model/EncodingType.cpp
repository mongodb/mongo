/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/iam/model/EncodingType.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace IAM
  {
    namespace Model
    {
      namespace EncodingTypeMapper
      {

        static const int SSH_HASH = HashingUtils::HashString("SSH");
        static const int PEM_HASH = HashingUtils::HashString("PEM");


        EncodingType GetEncodingTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == SSH_HASH)
          {
            return EncodingType::SSH;
          }
          else if (hashCode == PEM_HASH)
          {
            return EncodingType::PEM;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<EncodingType>(hashCode);
          }

          return EncodingType::NOT_SET;
        }

        Aws::String GetNameForEncodingType(EncodingType enumValue)
        {
          switch(enumValue)
          {
          case EncodingType::NOT_SET:
            return {};
          case EncodingType::SSH:
            return "SSH";
          case EncodingType::PEM:
            return "PEM";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace EncodingTypeMapper
    } // namespace Model
  } // namespace IAM
} // namespace Aws
