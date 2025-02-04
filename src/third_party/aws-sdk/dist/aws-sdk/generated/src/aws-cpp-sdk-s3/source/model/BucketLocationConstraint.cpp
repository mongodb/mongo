/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/model/BucketLocationConstraint.h>
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
      namespace BucketLocationConstraintMapper
      {

        static const int af_south_1_HASH = HashingUtils::HashString("af-south-1");
        static const int ap_east_1_HASH = HashingUtils::HashString("ap-east-1");
        static const int ap_northeast_1_HASH = HashingUtils::HashString("ap-northeast-1");
        static const int ap_northeast_2_HASH = HashingUtils::HashString("ap-northeast-2");
        static const int ap_northeast_3_HASH = HashingUtils::HashString("ap-northeast-3");
        static const int ap_south_1_HASH = HashingUtils::HashString("ap-south-1");
        static const int ap_south_2_HASH = HashingUtils::HashString("ap-south-2");
        static const int ap_southeast_1_HASH = HashingUtils::HashString("ap-southeast-1");
        static const int ap_southeast_2_HASH = HashingUtils::HashString("ap-southeast-2");
        static const int ap_southeast_3_HASH = HashingUtils::HashString("ap-southeast-3");
        static const int ca_central_1_HASH = HashingUtils::HashString("ca-central-1");
        static const int cn_north_1_HASH = HashingUtils::HashString("cn-north-1");
        static const int cn_northwest_1_HASH = HashingUtils::HashString("cn-northwest-1");
        static const int EU_HASH = HashingUtils::HashString("EU");
        static const int eu_central_1_HASH = HashingUtils::HashString("eu-central-1");
        static const int eu_north_1_HASH = HashingUtils::HashString("eu-north-1");
        static const int eu_south_1_HASH = HashingUtils::HashString("eu-south-1");
        static const int eu_south_2_HASH = HashingUtils::HashString("eu-south-2");
        static const int eu_west_1_HASH = HashingUtils::HashString("eu-west-1");
        static const int eu_west_2_HASH = HashingUtils::HashString("eu-west-2");
        static const int eu_west_3_HASH = HashingUtils::HashString("eu-west-3");
        static const int me_south_1_HASH = HashingUtils::HashString("me-south-1");
        static const int sa_east_1_HASH = HashingUtils::HashString("sa-east-1");
        static const int us_east_2_HASH = HashingUtils::HashString("us-east-2");
        static const int us_gov_east_1_HASH = HashingUtils::HashString("us-gov-east-1");
        static const int us_gov_west_1_HASH = HashingUtils::HashString("us-gov-west-1");
        static const int us_west_1_HASH = HashingUtils::HashString("us-west-1");
        static const int us_west_2_HASH = HashingUtils::HashString("us-west-2");
        static const int us_iso_west_1_HASH = HashingUtils::HashString("us-iso-west-1");
        static const int us_east_1_HASH = HashingUtils::HashString("us-east-1");


        BucketLocationConstraint GetBucketLocationConstraintForName(const Aws::String& name)
        {
          int hashCode = HashingUtils::HashString(name.c_str());
          if (hashCode == af_south_1_HASH)
          {
            return BucketLocationConstraint::af_south_1;
          }
          else if (hashCode == ap_east_1_HASH)
          {
            return BucketLocationConstraint::ap_east_1;
          }
          else if (hashCode == ap_northeast_1_HASH)
          {
            return BucketLocationConstraint::ap_northeast_1;
          }
          else if (hashCode == ap_northeast_2_HASH)
          {
            return BucketLocationConstraint::ap_northeast_2;
          }
          else if (hashCode == ap_northeast_3_HASH)
          {
            return BucketLocationConstraint::ap_northeast_3;
          }
          else if (hashCode == ap_south_1_HASH)
          {
            return BucketLocationConstraint::ap_south_1;
          }
          else if (hashCode == ap_south_2_HASH)
          {
            return BucketLocationConstraint::ap_south_2;
          }
          else if (hashCode == ap_southeast_1_HASH)
          {
            return BucketLocationConstraint::ap_southeast_1;
          }
          else if (hashCode == ap_southeast_2_HASH)
          {
            return BucketLocationConstraint::ap_southeast_2;
          }
          else if (hashCode == ap_southeast_3_HASH)
          {
            return BucketLocationConstraint::ap_southeast_3;
          }
          else if (hashCode == ca_central_1_HASH)
          {
            return BucketLocationConstraint::ca_central_1;
          }
          else if (hashCode == cn_north_1_HASH)
          {
            return BucketLocationConstraint::cn_north_1;
          }
          else if (hashCode == cn_northwest_1_HASH)
          {
            return BucketLocationConstraint::cn_northwest_1;
          }
          else if (hashCode == EU_HASH)
          {
            return BucketLocationConstraint::EU;
          }
          else if (hashCode == eu_central_1_HASH)
          {
            return BucketLocationConstraint::eu_central_1;
          }
          else if (hashCode == eu_north_1_HASH)
          {
            return BucketLocationConstraint::eu_north_1;
          }
          else if (hashCode == eu_south_1_HASH)
          {
            return BucketLocationConstraint::eu_south_1;
          }
          else if (hashCode == eu_south_2_HASH)
          {
            return BucketLocationConstraint::eu_south_2;
          }
          else if (hashCode == eu_west_1_HASH)
          {
            return BucketLocationConstraint::eu_west_1;
          }
          else if (hashCode == eu_west_2_HASH)
          {
            return BucketLocationConstraint::eu_west_2;
          }
          else if (hashCode == eu_west_3_HASH)
          {
            return BucketLocationConstraint::eu_west_3;
          }
          else if (hashCode == me_south_1_HASH)
          {
            return BucketLocationConstraint::me_south_1;
          }
          else if (hashCode == sa_east_1_HASH)
          {
            return BucketLocationConstraint::sa_east_1;
          }
          else if (hashCode == us_east_2_HASH)
          {
            return BucketLocationConstraint::us_east_2;
          }
          else if (hashCode == us_gov_east_1_HASH)
          {
            return BucketLocationConstraint::us_gov_east_1;
          }
          else if (hashCode == us_gov_west_1_HASH)
          {
            return BucketLocationConstraint::us_gov_west_1;
          }
          else if (hashCode == us_west_1_HASH)
          {
            return BucketLocationConstraint::us_west_1;
          }
          else if (hashCode == us_west_2_HASH)
          {
            return BucketLocationConstraint::us_west_2;
          }
          else if (hashCode == us_iso_west_1_HASH)
          {
            return BucketLocationConstraint::us_iso_west_1;
          }
          else if (hashCode == us_east_1_HASH)
          {
            return BucketLocationConstraint::us_east_1;
          }
          EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
          if(overflowContainer)
          {
            overflowContainer->StoreOverflow(hashCode, name);
            return static_cast<BucketLocationConstraint>(hashCode);
          }

          return BucketLocationConstraint::NOT_SET;
        }

        Aws::String GetNameForBucketLocationConstraint(BucketLocationConstraint enumValue)
        {
          switch(enumValue)
          {
          case BucketLocationConstraint::NOT_SET:
            return {};
          case BucketLocationConstraint::af_south_1:
            return "af-south-1";
          case BucketLocationConstraint::ap_east_1:
            return "ap-east-1";
          case BucketLocationConstraint::ap_northeast_1:
            return "ap-northeast-1";
          case BucketLocationConstraint::ap_northeast_2:
            return "ap-northeast-2";
          case BucketLocationConstraint::ap_northeast_3:
            return "ap-northeast-3";
          case BucketLocationConstraint::ap_south_1:
            return "ap-south-1";
          case BucketLocationConstraint::ap_south_2:
            return "ap-south-2";
          case BucketLocationConstraint::ap_southeast_1:
            return "ap-southeast-1";
          case BucketLocationConstraint::ap_southeast_2:
            return "ap-southeast-2";
          case BucketLocationConstraint::ap_southeast_3:
            return "ap-southeast-3";
          case BucketLocationConstraint::ca_central_1:
            return "ca-central-1";
          case BucketLocationConstraint::cn_north_1:
            return "cn-north-1";
          case BucketLocationConstraint::cn_northwest_1:
            return "cn-northwest-1";
          case BucketLocationConstraint::EU:
            return "EU";
          case BucketLocationConstraint::eu_central_1:
            return "eu-central-1";
          case BucketLocationConstraint::eu_north_1:
            return "eu-north-1";
          case BucketLocationConstraint::eu_south_1:
            return "eu-south-1";
          case BucketLocationConstraint::eu_south_2:
            return "eu-south-2";
          case BucketLocationConstraint::eu_west_1:
            return "eu-west-1";
          case BucketLocationConstraint::eu_west_2:
            return "eu-west-2";
          case BucketLocationConstraint::eu_west_3:
            return "eu-west-3";
          case BucketLocationConstraint::me_south_1:
            return "me-south-1";
          case BucketLocationConstraint::sa_east_1:
            return "sa-east-1";
          case BucketLocationConstraint::us_east_2:
            return "us-east-2";
          case BucketLocationConstraint::us_gov_east_1:
            return "us-gov-east-1";
          case BucketLocationConstraint::us_gov_west_1:
            return "us-gov-west-1";
          case BucketLocationConstraint::us_west_1:
            return "us-west-1";
          case BucketLocationConstraint::us_west_2:
            return "us-west-2";
          case BucketLocationConstraint::us_iso_west_1:
            return "us-iso-west-1";
          case BucketLocationConstraint::us_east_1:
            return "us-east-1";
          default:
            EnumParseOverflowContainer* overflowContainer = Aws::GetEnumOverflowContainer();
            if(overflowContainer)
            {
              return overflowContainer->RetrieveOverflow(static_cast<int>(enumValue));
            }

            return {};
          }
        }

      } // namespace BucketLocationConstraintMapper
    } // namespace Model
  } // namespace S3
} // namespace Aws
