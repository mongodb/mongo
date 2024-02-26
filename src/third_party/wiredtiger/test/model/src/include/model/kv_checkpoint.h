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

#include <memory>

#include "model/core.h"
#include "model/kv_transaction_snapshot.h"

namespace model {

/*
 * kv_checkpoint --
 *     A checkpoint in a key-value database.
 */
class kv_checkpoint {

public:
    /*
     * kv_checkpoint::kv_checkpoint --
     *     Create a new instance of the checkpoint.
     */
    inline kv_checkpoint(
      const char *name, kv_transaction_snapshot_ptr snapshot, timestamp_t stable_timestamp) noexcept
        : _name(name), _snapshot(snapshot), _stable_timestamp(stable_timestamp)
    {
    }

    /*
     * kv_checkpoint::name --
     *     Get the name of the checkpoint. The lifetime of the returned pointer follows the lifetime
     *     of this object (given that this is a pointer to a read-only field in this class). We
     *     return this as a regular C pointer so that it can be easily used in C APIs.
     */
    inline const char *
    name() const noexcept
    {
        return _name.c_str();
    }

    /*
     * kv_transaction::snapshot --
     *     Get the transaction snapshot.
     */
    inline kv_transaction_snapshot_ptr
    snapshot() const noexcept
    {
        return _snapshot;
    }

    /*
     * kv_transaction::stable_timestamp --
     *     Get the checkpoint's stable timestamp, if set.
     */
    inline timestamp_t
    stable_timestamp() const noexcept
    {
        return _stable_timestamp;
    }

private:
    kv_transaction_snapshot_ptr _snapshot;
    timestamp_t _stable_timestamp;
    std::string _name;
};

/*
 * kv_checkpoint_ptr --
 *     A shared pointer to the checkpoint.
 */
using kv_checkpoint_ptr = std::shared_ptr<kv_checkpoint>;

} /* namespace model */
