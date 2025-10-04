/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <aws/s3/S3_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
namespace S3
{
namespace Model
{
  enum class BucketLocationConstraint
  {
    NOT_SET,
    af_south_1,
    ap_east_1,
    ap_northeast_1,
    ap_northeast_2,
    ap_northeast_3,
    ap_south_1,
    ap_south_2,
    ap_southeast_1,
    ap_southeast_2,
    ap_southeast_3,
    ca_central_1,
    cn_north_1,
    cn_northwest_1,
    EU,
    eu_central_1,
    eu_north_1,
    eu_south_1,
    eu_south_2,
    eu_west_1,
    eu_west_2,
    eu_west_3,
    me_south_1,
    sa_east_1,
    us_east_2,
    us_gov_east_1,
    us_gov_west_1,
    us_west_1,
    us_west_2,
    us_iso_west_1,
    us_east_1
  };

namespace BucketLocationConstraintMapper
{
AWS_S3_API BucketLocationConstraint GetBucketLocationConstraintForName(const Aws::String& name);

AWS_S3_API Aws::String GetNameForBucketLocationConstraint(BucketLocationConstraint value);
} // namespace BucketLocationConstraintMapper
} // namespace Model
} // namespace S3
} // namespace Aws
