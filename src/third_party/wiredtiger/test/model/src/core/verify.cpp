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

#include <algorithm>
#include <iostream>
#include <sstream>

#include "model/kv_table.h"
#include "model/util.h"
#include "model/verify.h"
#include "wiredtiger.h"

namespace model {

/*
 * kv_table_verify_cursor::has_next --
 *     Determine whether the cursor has a next value.
 */
bool
kv_table_verify_cursor::has_next()
{
    auto i = _iterator;

    /* Skip over any deleted items. */
    while (i != _data.end() && i->second.get() == NONE)
        i++;

    return i != _data.end();
}

/*
 * kv_table_verify_cursor::verify_next --
 *     Verify the next key-value pair. This method is not thread-safe.
 */
bool
kv_table_verify_cursor::verify_next(const data_value &key, const data_value &value)
{
    /* If we have reached the end of the model's state, we failed. */
    if (_iterator == _data.end())
        return false;

    /* Skip over any deleted items. */
    while (_iterator != _data.end() && _iterator->second.get() == NONE)
        _iterator++;
    if (_iterator == _data.end())
        return false;

    /* Advance the iterator, but keep the current position for the rest of this function. */
    auto i = _iterator;
    _iterator++;

    /* Check the key. */
    if (key != i->first)
        return false;

    /* Check the value. */
    return i->second.contains_any(value);
}

/*
 * kv_table_verifier::verify --
 *     Verify the table by comparing a WiredTiger table against the model. Throw an exception on
 *     error.
 */
void
kv_table_verifier::verify(WT_CONNECTION *connection)
{
    WT_SESSION *session = nullptr;
    WT_CURSOR *wt_cursor = nullptr;
    int ret;
    const char *key, *value;

    if (_verbose)
        std::cout << "Verification: Verify " << _table.name() << std::endl;

    /* Get the model cursor. */
    kv_table_verify_cursor model_cursor = _table.verify_cursor();

    try {
        /* Get the database cursor. */
        ret = connection->open_session(connection, nullptr, nullptr, &session);
        if (ret != 0)
            throw wiredtiger_exception(ret);

        /* Automatically close the session at the end of the block. */
        wiredtiger_session_guard session_guard(session);

        std::string uri = std::string("table:") + _table.name();
        ret = session->open_cursor(session, uri.c_str(), nullptr, nullptr, &wt_cursor);
        if (ret != 0)
            throw wiredtiger_exception(session, ret);

        /* Automatically close the cursor at the end of the block. */
        wiredtiger_cursor_guard cursor_guard(wt_cursor);

        /* Verify each key-value pair. */
        while ((ret = wt_cursor->next(wt_cursor)) == 0) {
            ret = wt_cursor->get_key(wt_cursor, &key);
            if (ret != 0)
                throw wiredtiger_exception(session, ret);
            ret = wt_cursor->get_value(wt_cursor, &value);
            if (ret != 0)
                throw wiredtiger_exception(session, ret);
            if (_verbose)
                std::cout << "Verification: key = " << key << ", value = " << value << std::endl;
            if (!model_cursor.verify_next(data_value(key), data_value(value))) {
                std::ostringstream ss;
                ss << "\"" << key << "=" << value
                   << "\" is not the next key-value pair in the model.";
                throw verify_exception(ss.str());
            }
        }

        /* Make sure that we reached the end at the same time. */
        if (_verbose)
            std::cout << "Verification: Reached the end." << std::endl;
        if (ret != WT_NOTFOUND)
            throw wiredtiger_exception(session, "Advancing the cursor failed", ret);
        if (model_cursor.has_next())
            throw verify_exception("There are still more key-value pairs in the model.");
        if (_verbose)
            std::cout << "Verification: Finished." << std::endl;

    } catch (std::exception &e) {
        if (_verbose)
            std::cerr << "Verification Failed: " << e.what() << std::endl;
        throw;
    }

    /* No need to clean up the session or the cursor due to the use of guards. */
}

} /* namespace model */
