/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/State.h>
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
      namespace StateMapper
      {

        static const int Pending_HASH = HashingUtils::HashString("Pending");
        static const int Active_HASH = HashingUtils::HashString("Active");
        static const int Inactive_HASH = HashingUtils::HashString("Inactive");
        static const int Failed_HASH = HashingUtils::HashString("Failed");


        State GetStateForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == Pending_HASH)
          {
            return State::Pending;
          }
          else if (hashCode == Active_HASH)
          {
            return State::Active;
          }
          else if (hashCode == Inactive_HASH)
          {
            return State::Inactive;
          }
          else if (hashCode == Failed_HASH)
          {
            return State::Failed;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<State>(hashCode);
          }

          return State::NOT_SET;
        }

        Aws::String GetNameForState(State enumValue)
        {
          switch(enumValue)
          {
          case State::NOT_SET:
            return {};
          case State::Pending:
            return "Pending";
          case State::Active:
            return "Active";
          case State::Inactive:
            return "Inactive";
          case State::Failed:
            return "Failed";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace StateMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
