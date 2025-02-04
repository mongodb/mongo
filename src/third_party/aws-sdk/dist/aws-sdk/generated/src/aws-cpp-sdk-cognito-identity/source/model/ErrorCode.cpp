/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/cognito-identity/model/ErrorCode.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/Globals.h>
#include <aws/core/utils/EnumParseOverflowContainer.h>

using namespace Aws::Utils;


namespace Aws
{
  namespace CognitoIdentity
  {
    namespace Model
    {
      namespace ErrorCodeMapper
      {

        static const int AccessDenied_HASH = HashingUtils::HashString("AccessDenied");
        static const int InternalServerError_HASH = HashingUtils::HashString("InternalServerError");


        ErrorCode GetErrorCodeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == AccessDenied_HASH)
          {
            return ErrorCode::AccessDenied;
          }
          else if (hashCode == InternalServerError_HASH)
          {
            return ErrorCode::InternalServerError;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ErrorCode>(hashCode);
          }

          return ErrorCode::NOT_SET;
        }

        Aws::String GetNameForErrorCode(ErrorCode enumValue)
        {
          switch(enumValue)
          {
          case ErrorCode::NOT_SET:
            return {};
          case ErrorCode::AccessDenied:
            return "AccessDenied";
          case ErrorCode::InternalServerError:
            return "InternalServerError";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ErrorCodeMapper
    } // namespace Model
  } // namespace CognitoIdentity
} // namespace Aws
