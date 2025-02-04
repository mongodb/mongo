/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/platform/Time.h>

#include <time.h>

namespace Aws
{
namespace Time
{

time_t TimeGM(struct tm* const t)
{
    return timegm(t);
}

void LocalTime(tm* t, std::time_t time)
{
    localtime_r(&time, t);
}

void GMTime(tm* t, std::time_t time)
{
    gmtime_r(&time, t);
}

} // namespace Time
} // namespace Aws
