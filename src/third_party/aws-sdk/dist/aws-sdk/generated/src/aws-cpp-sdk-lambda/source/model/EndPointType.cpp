/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/lambda/model/EndPointType.h>
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
      namespace EndPointTypeMapper
      {

        static const int KAFKA_BOOTSTRAP_SERVERS_HASH = HashingUtils::HashString("KAFKA_BOOTSTRAP_SERVERS");


        EndPointType GetEndPointTypeForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == KAFKA_BOOTSTRAP_SERVERS_HASH)
          {
            return EndPointType::KAFKA_BOOTSTRAP_SERVERS;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<EndPointType>(hashCode);
          }

          return EndPointType::NOT_SET;
        }

        Aws::String GetNameForEndPointType(EndPointType enumValue)
        {
          switch(enumValue)
          {
          case EndPointType::NOT_SET:
            return {};
          case EndPointType::KAFKA_BOOTSTRAP_SERVERS:
            return "KAFKA_BOOTSTRAP_SERVERS";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace EndPointTypeMapper
    } // namespace Model
  } // namespace Lambda
} // namespace Aws
