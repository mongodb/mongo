/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/JSONType.h>
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
      namespace JSONTypeMapper
      {

        static const int DOCUMENT_HASH = HashingUtils::HashString("DOCUMENT");
        static const int LINES_HASH = HashingUtils::HashString("LINES");


        JSONType GetJSONTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == DOCUMENT_HASH)
          {
            return JSONType::DOCUMENT;
          }
          else if (hashCode == LINES_HASH)
          {
            return JSONType::LINES;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<JSONType>(hashCode);
          }

          return JSONType::NOT_SET;
        }

        Aws::String GetNameForJSONType(JSONType enumValue)
        {
          switch(enumValue)
          {
          case JSONType::NOT_SET:
            return {};
          case JSONType::DOCUMENT:
            return "DOCUMENT";
          case JSONType::LINES:
            return "LINES";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace JSONTypeMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
