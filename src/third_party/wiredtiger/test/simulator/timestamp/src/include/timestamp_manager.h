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

#pragma once

#include <map>
#include <string>
#include <vector>

#include "session_simulator.h"

/* Timestamp is a global singleton class responsible for validating the timestamps. */
class timestamp_manager {
    /* Methods */
public:
    static timestamp_manager &get_timestamp_manager();
    int parse_config(const std::string &, const std::vector<std::string> &,
      const std::vector<std::string> &, std::map<std::string, std::string> &);
    uint64_t hex_to_decimal(const std::string &);
    std::string decimal_to_hex(const uint64_t);
    int validate_hex_value(const std::string &, const std::string &);

    /* Methods for validating timestamps */
public:
    int validate_oldest_and_stable_timestamp(uint64_t &, uint64_t &, bool &, bool &);
    int validate_conn_durable_timestamp(const uint64_t &, const bool &) const;
    int validate_read_timestamp(session_simulator *, const uint64_t) const;
    int validate_commit_timestamp(session_simulator *, uint64_t);
    int validate_prepare_timestamp(session_simulator *, uint64_t) const;
    int validate_session_durable_timestamp(session_simulator *, uint64_t);

private:
    std::string trim(std::string);

    /* No copies of the singleton allowed. */
private:
    timestamp_manager();

public:
    timestamp_manager(timestamp_manager const &) = delete;
    timestamp_manager &operator=(timestamp_manager const &) = delete;
};
