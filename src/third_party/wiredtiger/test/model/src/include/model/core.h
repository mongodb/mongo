/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef MODEL_CORE_H
#define MODEL_CORE_H

#include <limits>

/* Redefine important WiredTiger internal constants, if they are not already available. */

/*
 * WT_TS_MAX --
 *     The maximum timestamp, typically used in reads where we would like to get the latest value.
 */
#ifndef WT_TS_MAX
#define WT_TS_MAX UINT64_MAX
#endif

/*
 * WT_TS_NONE --
 *     No timestamp, e.g., when performing a non-timestamped update.
 */
#ifndef WT_TS_NONE
#define WT_TS_NONE 0
#endif

namespace model {

/*
 * timestamp_t --
 *     The timestamp. This is the model's equivalent of wt_timestamp_t.
 */
using timestamp_t = uint64_t;

/*
 * k_timestamp_none --
 *     No timestamp, e.g., when performing a non-timestamped update.
 */
constexpr timestamp_t k_timestamp_none = std::numeric_limits<timestamp_t>::min();

/*
 * k_timestamp_max --
 *     The maximum timestamp, typically used in reads where we would like to get the latest value.
 */
constexpr timestamp_t k_timestamp_max = std::numeric_limits<timestamp_t>::max();

/*
 * k_timestamp_latest --
 *     A convenience alias for k_timestamp_max, typically used to get the latest value.
 */
constexpr timestamp_t k_timestamp_latest = k_timestamp_max;

/* Verify that model's constants are numerically equal to WiredTiger's constants. */
static_assert(k_timestamp_latest == WT_TS_MAX);
static_assert(k_timestamp_max == WT_TS_MAX);
static_assert(k_timestamp_none == WT_TS_NONE);

} /* namespace model */
#endif
