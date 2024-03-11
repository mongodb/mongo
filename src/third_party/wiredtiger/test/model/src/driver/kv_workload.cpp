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

#include <string>
#include <string_view>

extern "C" {
#include "wt_internal.h"
}

#include "model/driver/kv_workload.h"
#include "model/driver/kv_workload_runner.h"
#include "model/driver/kv_workload_runner_wt.h"
#include "model/util.h"

namespace model {

namespace operation {

/*
 * parse --
 *     Parse an operation from a string. Throw an exception on error.
 */
any
parse(const char *str)
{
    /* Get the operation name. */
    size_t i = 0;
    while (std::isalnum(str[i]) || str[i] == '_')
        ++i;
    std::string_view name(str, i);

    const char *p = str + i;
    while (std::isspace(*p))
        ++p;
    if (*(p++) != '(')
        throw model_exception("Expected '('");

    /* Get the arguments. */
    std::vector<std::string> args;
    std::string current;
    bool done = false;
    bool escaped = false;
    bool had_quotes = false;
    bool quotes = false;
    while (*p != '\0') {
        char c = *(p++);

        if (escaped) {
            current += c;
            escaped = false;
            continue;
        }

        if (quotes) {
            if (c == '\"') {
                quotes = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            current += c;
            continue;
        }

        if (std::isspace(c))
            continue;

        if (c == '\"') {
            quotes = true;
            had_quotes = true;
            continue;
        }

        if (c == ',' || c == ')') {
            /* An empty string must have quotes if it is not the last argument. */
            if (!current.empty() || had_quotes || c == ',')
                args.push_back(current);
            current = "";
            had_quotes = false;
            if (c == ')') {
                done = true;
                break;
            }
            continue;
        }

        current += c;
    }

    if (!done)
        throw model_exception("Expected ')'");
    while (std::isspace(*p))
        ++p;
    if (*(p++) != '\0')
        throw model_exception("Extra characters at the end of the string.");

#define CHECK_NUM_ARGS(expected)   \
    if (args.size() != (expected)) \
        throw model_exception("Expected " + std::to_string(expected) + " arguments");
#define CHECK_NUM_ARGS_RANGE(min, max)                                              \
    if (args.size() < min || args.size() > max)                                     \
        throw model_exception("Expected between " + std::to_string(min) + " and " + \
          std::to_string(max) + " arguments");

    /* Now actually parse the operation. */
    /* FIXME: We currently only support unsigned numbers for keys and values. */
    if (name == "begin_transaction") {
        CHECK_NUM_ARGS(1);
        return begin_transaction(parse_uint64(args[0]));
    }
    if (name == "checkpoint") {
        CHECK_NUM_ARGS_RANGE(0, 1);
        return checkpoint(args.size() == 0 ? nullptr : args[0].c_str());
    }
    if (name == "commit_transaction") {
        CHECK_NUM_ARGS_RANGE(1, 3);
        return commit_transaction(parse_uint64(args[0]),
          args.size() <= 1 ? k_timestamp_none : parse_uint64(args[1]),
          args.size() <= 2 ? k_timestamp_none : parse_uint64(args[2]));
    }
    if (name == "crash") {
        CHECK_NUM_ARGS(0);
        return crash();
    }
    if (name == "create_table") {
        CHECK_NUM_ARGS(4);
        return create_table(
          parse_uint64(args[0]), args[1].c_str(), args[2].c_str(), args[3].c_str());
    }
    if (name == "insert") {
        CHECK_NUM_ARGS(4);
        return insert(parse_uint64(args[0]), parse_uint64(args[1]),
          data_value(parse_uint64(args[2])), data_value(parse_uint64(args[3])));
    }
    if (name == "prepare_transaction") {
        CHECK_NUM_ARGS(2);
        return prepare_transaction(parse_uint64(args[0]), parse_uint64(args[1]));
    }
    if (name == "remove") {
        CHECK_NUM_ARGS(3);
        return remove(
          parse_uint64(args[0]), parse_uint64(args[1]), data_value(parse_uint64(args[2])));
    }
    if (name == "restart") {
        CHECK_NUM_ARGS(0);
        return restart();
    }
    if (name == "rollback_to_stable") {
        CHECK_NUM_ARGS(0);
        return rollback_to_stable();
    }
    if (name == "rollback_transaction") {
        CHECK_NUM_ARGS(1);
        return rollback_transaction(parse_uint64(args[0]));
    }
    if (name == "set_commit_timestamp") {
        CHECK_NUM_ARGS(2);
        return set_commit_timestamp(parse_uint64(args[0]), parse_uint64(args[1]));
    }
    if (name == "set_stable_timestamp") {
        CHECK_NUM_ARGS(1);
        return set_stable_timestamp(parse_uint64(args[0]));
    }
    if (name == "truncate") {
        CHECK_NUM_ARGS(4);
        return truncate(parse_uint64(args[0]), parse_uint64(args[1]),
          data_value(parse_uint64(args[2])), data_value(parse_uint64(args[3])));
    }

#undef CHECK_NUM_ARGS
#undef CHECK_NUM_ARGS_RANGE

    throw model_exception(
      "Cannot parse operation: Unknown operation \"" + std::string(name) + "\"");
}

} /* namespace operation */

/*
 * kv_workload::run --
 *     Run the workload in the model. Return the return codes of the workload operations.
 */
std::vector<int>
kv_workload::run(kv_database &database) const
{
    kv_workload_runner runner{database};
    return runner.run(*this);
}

/*
 * kv_workload::run_in_wiredtiger --
 *     Run the workload in WiredTiger. Return the return codes of the workload operations.
 */
std::vector<int>
kv_workload::run_in_wiredtiger(
  const char *home, const char *connection_config, const char *table_config) const
{
    kv_workload_runner_wt runner{home, connection_config, table_config};
    return runner.run(*this);
}

} /* namespace model */
