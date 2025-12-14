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

#include <string>

#include "src/storage/scoped_session.h"
extern "C" {
#include "wiredtiger.h"
}

namespace test_harness {

class transaction {
public:
    bool active() const;
    void begin(scoped_session &session, const std::string &config = "");
    /*
     * Commit a transaction and return true if the commit was successful.
     */
    bool commit(scoped_session &session, const std::string &config = "");
    /* Rollback a transaction, failure will abort the test. */
    void rollback(scoped_session &session, const std::string &config = "");
    /* Set that the transaction needs to be rolled back. */
    void set_needs_rollback();
    /* Return whether the transaction needs to be rolled back.*/
    bool needs_rollback();

private:
    bool _in_txn = false;
    bool _needs_rollback = false;
};

} // namespace test_harness
