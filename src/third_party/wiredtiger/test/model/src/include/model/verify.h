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

#include <stdexcept>
#include <string>
#include <utility>

#include "model/data_value.h"
#include "wiredtiger.h"

namespace model {

class kv_table;

/*
 * verify_exception --
 *     The verification exception.
 */
class verify_exception : public std::runtime_error {

public:
    /*
     * verify_exception::verify_exception --
     *     Create a new instance of the exception.
     */
    inline verify_exception(const std::string &what) : std::runtime_error(what) {}
};

/*
 * kv_table_verify_cursor --
 *     The verification cursor
 */
class kv_table_verify_cursor {

public:
    /*
     * kv_table_verify_cursor::kv_table_verify_cursor --
     *     Create a new instance of the verification cursor.
     */
    inline kv_table_verify_cursor(std::map<data_value, kv_table_item> &data) noexcept
        : _data(data), _iterator(_data.begin())
    {
    }

    /*
     * kv_table_verify_cursor::has_next --
     *     Determine whether the cursor has a next value.
     */
    bool has_next();

    /*
     * kv_table_verify_cursor::set_checkpoint --
     *     Set the checkpoint for verifying the table. This can be called only at the beginning.
     */
    inline void
    set_checkpoint(kv_checkpoint_ptr ckpt)
    {
        if (_iterator != _data.begin())
            throw model_exception("The cursor is not at the beginning");
        _ckpt = ckpt;
    }

    /*
     * kv_table_verify_cursor::verify_next --
     *     Verify the next key-value pair. This method is not thread-safe.
     */
    bool verify_next(const data_value &key, const data_value &value);

    /*
     * kv_table_verify_cursor::get_prev --
     *     Get the previous key-value pair, but do not move the iterator. This method is not
     *     thread-safe.
     */
    std::pair<data_value, data_value> get_prev() const;

private:
    std::map<data_value, kv_table_item> &_data;
    std::map<data_value, kv_table_item>::iterator _iterator;
    kv_checkpoint_ptr _ckpt;
};

/*
 * kv_table_verifier --
 *     Table verification.
 */
class kv_table_verifier {

public:
    /*
     * kv_table_verifier::kv_table_verifier --
     *     Create a new instance of the verifier.
     */
    inline kv_table_verifier(kv_table &table) noexcept : _table(table), _verbose(false) {}

    /*
     * kv_table_verifier::verify --
     *     Verify the table by comparing a WiredTiger table against the model, with or without using
     *     a checkpoint. Throw an exception on error.
     */
    void verify(WT_CONNECTION *connection, kv_checkpoint_ptr ckpt = kv_checkpoint_ptr(nullptr));

    /*
     * kv_table_verifier::verify --
     *     Verify the table by comparing a WiredTiger table against the model. Does not throw
     *     exceptions, but simply returns a boolean. This is useful for model's own unit testing.
     */
    inline bool
    verify_noexcept(
      WT_CONNECTION *connection, kv_checkpoint_ptr ckpt = kv_checkpoint_ptr(nullptr)) noexcept
    {
        try {
            verify(connection, ckpt);
        } catch (...) {
            return false;
        }
        return true;
    }

private:
    kv_table &_table;
    bool _verbose;
};

} /* namespace model */
