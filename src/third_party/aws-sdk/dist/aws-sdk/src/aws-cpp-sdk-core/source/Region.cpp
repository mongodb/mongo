/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/Region.h>
namespace Aws
{
    namespace Region
    {
        Aws::String ComputeSignerRegion(const Aws::String& region)
        {
            if (region == Aws::Region::AWS_GLOBAL)
            {
                return Aws::Region::US_EAST_1;
            }
            else if (region == "fips-aws-global")
            {
                return Aws::Region::US_EAST_1;
            }
            else if (region == "s3-external-1")
            {
                return Aws::Region::US_EAST_1;
            }
            else if (region.size() >= 5 && region.compare(0, 5, "fips-") == 0)
            {
                return region.substr(5);
            }
            else if (region.size() >= 5 && region.compare(region.size() - 5, 5, "-fips") == 0)
            {
                return region.substr(0, region.size() - 5);
            }
            else
            {
                return region;
            }
        }

        bool IsFipsRegion(const Aws::String& region)
        {
            if (region.size() >= 5 && region.compare(0, 5, "fips-") == 0)
            {
                return true;
            }
            else if (region.size() >= 5 && region.compare(region.size() - 5, 5, "-fips") == 0)
            {
                return true;
            }
            return false;
        }
    }
}