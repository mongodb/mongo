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

#include <cstdint>
#include <iostream>
#include <algorithm>
#include <cmath>

#include "instruction_counter.h"
#include "src/component/metrics_writer.h"

namespace test_harness {
instruction_counter::instruction_counter(const std::string &id, const std::string &test_name)
    : _id(id), _test_name(test_name), _instruction_count(0)
{
    memset(&_pe, 0, sizeof(_pe));
    _pe.type = PERF_TYPE_HARDWARE;
    _pe.size = sizeof(_pe);
    _pe.config = PERF_COUNT_HW_INSTRUCTIONS;
    _pe.disabled = 1;
    _pe.exclude_kernel = 1;
    _pe.exclude_hv = 1;
}

void
instruction_counter::append_stats()
{
    metrics_writer::instance().add_stat("{\"name\":\"" + _id +
      "_instructions\",\"value\":" + std::to_string(_instruction_count) + "}");
}

instruction_counter::~instruction_counter()
{
    append_stats();
}
}; // namespace test_harness
