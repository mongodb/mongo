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

#include "operation_configuration.h"

#include "src/common/constants.h"

namespace test_harness {
operation_configuration::operation_configuration(configuration *config, thread_type type)
    : config(config), type(type), thread_count(config->get_int(THREAD_COUNT))
{
}

std::function<void(thread_worker *)>
operation_configuration::get_func(database_operation *dbo)
{
    switch (type) {
    case thread_type::BACKGROUND_COMPACT:
        return (
          std::bind(&database_operation::background_compact_operation, dbo, std::placeholders::_1));
    case thread_type::CHECKPOINT:
        return (std::bind(&database_operation::checkpoint_operation, dbo, std::placeholders::_1));
    case thread_type::CUSTOM:
        return (std::bind(&database_operation::custom_operation, dbo, std::placeholders::_1));
    case thread_type::INSERT:
        return (std::bind(&database_operation::insert_operation, dbo, std::placeholders::_1));
    case thread_type::READ:
        return (std::bind(&database_operation::read_operation, dbo, std::placeholders::_1));
    case thread_type::REMOVE:
        return (std::bind(&database_operation::remove_operation, dbo, std::placeholders::_1));
    case thread_type::UPDATE:
        return (std::bind(&database_operation::update_operation, dbo, std::placeholders::_1));
    }
    testutil_die(EINVAL, "unexpected thread_type: %d", static_cast<int>(type));
}
} // namespace test_harness
