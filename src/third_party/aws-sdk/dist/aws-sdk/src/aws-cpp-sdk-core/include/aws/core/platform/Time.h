/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <ctime>

namespace Aws
{
namespace Time
{

    /*
    * A platform-agnostic implementation of the timegm function from gnu extensions
    */
    AWS_CORE_API time_t TimeGM(tm* const t);

    /*
    * Converts from a time value of std::time_t type to a C tm structure for easier date analysis
    */
    AWS_CORE_API void LocalTime(tm* t, std::time_t time);

    /*
    * Converts from a time value of std::time_t type to a C tm structure for easier date analysis
    */
    AWS_CORE_API void GMTime(tm* t, std::time_t time);

} // namespace Time
} // namespace Aws
