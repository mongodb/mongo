/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/kinesis/model/ConsumerStatus.h>
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
      namespace ConsumerStatusMapper
      {

        static const int CREATING_HASH = HashingUtils::HashString("CREATING");
        static const int DELETING_HASH = HashingUtils::HashString("DELETING");
        static const int ACTIVE_HASH = HashingUtils::HashString("ACTIVE");


        ConsumerStatus GetConsumerStatusForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == CREATING_HASH)
          {
            return ConsumerStatus::CREATING;
          }
          else if (hashCode == DELETING_HASH)
          {
            return ConsumerStatus::DELETING;
          }
          else if (hashCode == ACTIVE_HASH)
          {
            return ConsumerStatus::ACTIVE;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<ConsumerStatus>(hashCode);
          }

          return ConsumerStatus::NOT_SET;
        }

        Aws::String GetNameForConsumerStatus(ConsumerStatus enumValue)
        {
          switch(enumValue)
          {
          case ConsumerStatus::NOT_SET:
            return {};
          case ConsumerStatus::CREATING:
            return "CREATING";
          case ConsumerStatus::DELETING:
            return "DELETING";
          case ConsumerStatus::ACTIVE:
            return "ACTIVE";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace ConsumerStatusMapper
    } // namespace Model
  } // namespace Kinesis
} // namespace Aws
