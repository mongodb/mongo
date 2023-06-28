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

#include "connection_simulator.h"

#define RESET "\033[0m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define WHITE "\033[37m"

/* Helper functions. */
int choose_num(int, int, const std::string &);
void print_border_msg(const std::string &, const std::string &);
void print_options(const std::vector<std::string> &);
const std::string get_input(const std::string &);
session_simulator *get_session(
  const std::map<std::string, session_simulator *> &, const std::string &);

/* session management method. */
void interface_session_management(
  connection_simulator *, std::map<std::string, session_simulator *> &, std::string &);

/* connection level methods. */
void interface_set_timestamp(connection_simulator *);
void interface_conn_query_timestamp(connection_simulator *);

/* session level methods. */
void interface_begin_transaction(session_simulator *);
void interface_commit_transaction(session_simulator *);
void interface_prepare_transaction(session_simulator *);
void interface_rollback_transaction(session_simulator *);
void interface_timestamp_transaction(session_simulator *);
void interface_session_query_timestamp(session_simulator *);

/* Print rules for timestamps. */
void print_rules();
