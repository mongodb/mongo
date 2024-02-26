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

extern "C" {
#include "wt_internal.h"
}

#include "model/driver/kv_workload.h"
#include "model/driver/kv_workload_runner.h"
#include "model/driver/kv_workload_runner_wt.h"
#include "model/util.h"

namespace model {

/*
 * kv_workload::run --
 *     Run the workload in the model.
 */
void
kv_workload::run(kv_database &database) const
{
    kv_workload_runner runner{database};
    runner.run(*this);
}

/*
 * kv_workload::run_in_wiredtiger --
 *     Run the workload in WiredTiger.
 */
void
kv_workload::run_in_wiredtiger(
  const char *home, const char *connection_config, const char *table_config) const
{
    kv_workload_runner_wt runner{home, connection_config, table_config};
    runner.run(*this);
}

} /* namespace model */
