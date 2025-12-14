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

#include "src/storage/scoped_cursor.h"
#include "transaction.h"

extern "C" {
#include "wiredtiger.h"
}

namespace test_harness {
namespace crud {
inline bool
insert(scoped_cursor &cursor, transaction &txn, const std::string &key, const std::string &value)
{
    cursor->set_key(cursor.get(), key.c_str());
    cursor->set_value(cursor.get(), value.c_str());
    int ret = cursor->insert(cursor.get());

    if (ret != 0) {
        if (ret == WT_ROLLBACK) {
            txn.set_needs_rollback();
            return (false);
        } else
            testutil_die(ret, "unhandled error while trying to insert a key");
    }
    return (true);
}
inline bool
update(scoped_cursor &cursor, transaction &txn, const std::string &key, const std::string &value)
{
    cursor->set_key(cursor.get(), key.c_str());
    cursor->set_value(cursor.get(), value.c_str());
    int ret = cursor->update(cursor.get());

    if (ret != 0) {
        if (ret == WT_ROLLBACK) {
            txn.set_needs_rollback();
            return (false);
        } else
            testutil_die(ret, "unhandled error while trying to update a key");
    }
    return (true);
}
inline bool
remove(scoped_cursor &cursor, transaction &txn, const std::string &key)
{
    cursor->set_key(cursor.get(), key.c_str());
    int ret = cursor->remove(cursor.get());
    if (ret != 0) {
        if (ret == WT_ROLLBACK) {
            txn.set_needs_rollback();
            return (false);
        } else
            testutil_die(ret, "unhandled error while trying to remove a key");
    }
    return (true);
}
}; // namespace crud
} // namespace test_harness
