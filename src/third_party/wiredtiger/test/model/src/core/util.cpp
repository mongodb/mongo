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

#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

#include "wiredtiger.h"
extern "C" {
#include "wt_internal.h"
}

#include "model/util.h"

namespace model {

/*
 * config_map::from_string --
 *     Parse config map from a string.
 */
config_map
config_map::from_string(const char *str, const char **end)
{
    std::ostringstream key_buf, value_buf;
    bool in_key, in_quotes;
    config_map m;
    const char *p;

    in_key = true;
    in_quotes = false;

    for (p = str; *p != '\0' && (in_quotes || *p != ')'); p++) {
        char c = *p;

        /* Handle quotes. */
        if (in_quotes) {
            if (c == '\"') {
                in_quotes = false;
                continue;
            }
        } else if (c == '\"') {
            in_quotes = true;
            continue;
        }

        if (in_key) {
            /* Handle keys. */
            if (!in_quotes && c == '=')
                in_key = false;
            else
                key_buf << c;
        } else {
            /* Handle nested config maps. */
            if (!in_quotes && c == '(') {
                if (value_buf.str() != "")
                    throw model_exception("Invalid nested configuration string");
                m._map[key_buf.str()] = std::make_shared<config_map>(from_string(p + 1, &p));
                if (*p != ')')
                    throw model_exception("Invalid nesting within a configuration string");
                key_buf.str("");
                in_key = true;
            }
            /* Handle regular values. */
            else if (!in_quotes && c == ',') {
                m._map[key_buf.str()] = value_buf.str();
                key_buf.str("");
                value_buf.str("");
                in_key = true;
            }
            /* Else we just get the next character. */
            else
                value_buf << c;
        }
    }

    /* Handle the last value. */
    if (in_quotes)
        throw model_exception("Unmatched quotes within a configuration string");
    if (!in_key)
        m._map[key_buf.str()] = value_buf.str();

    /* Handle the end of a nested map. */
    if (end == NULL) {
        if (*p != '\0')
            throw model_exception("Invalid configuration string");
    } else
        *end = p;

    return m;
}

/*
 * wt_list_tables --
 *     Get the list of WiredTiger tables.
 */
std::vector<std::string>
wt_list_tables(WT_CONNECTION *conn)
{
    int ret;
    std::vector<std::string> tables;

    WT_SESSION *session;
    ret = conn->open_session(conn, nullptr, nullptr, &session);
    if (ret != 0)
        throw wiredtiger_exception("Cannot open a session: ", ret);
    wiredtiger_session_guard session_guard(session);

    WT_CURSOR *cursor;
    ret = session->open_cursor(session, WT_METADATA_URI, NULL, NULL, &cursor);
    if (ret != 0)
        throw wiredtiger_exception("Cannot open a metadata cursor: ", ret);
    wiredtiger_cursor_guard cursor_guard(cursor);

    const char *key;
    while ((ret = cursor->next(cursor)) == 0) {
        /* Get the key. */
        if ((ret = cursor->get_key(cursor, &key)) != 0)
            throw wiredtiger_exception("Cannot get key: ", ret);

        if (strncmp(key, "table:", 6) == 0)
            tables.push_back(key + 6);
    }

    return tables;
}

} /* namespace model */
