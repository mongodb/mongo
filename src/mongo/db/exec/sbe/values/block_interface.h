/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>

#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe::value {
/**
 * Interface for accessing a collection of SBE Values independent of their backing storage.
 *
 * Currently we only support forward iteration over all of the values via 'Cursor' but PM-3168
 * will extend the interface to allow for other operations to be applied which may run directly on
 * the underlying format or take advantage of precomputed summaries.
 */
struct ValueBlock {
    /**
     * Used for iterating over the whole block.
     */
    struct Cursor {
        virtual ~Cursor() = default;

        /**
         * Returns unowned value and might invalidate the previously returned value(s). If the
         * client needs to maintain a value across multiple calls to 'next()' they should copy it
         * and assume ownership of the copy. Returning 'boost::none' represents the end of data in
         * this block. It is allowed to call 'next()' in this state and it would continue returning
         * 'boost::none'.
         */
        virtual boost::optional<std::pair<TypeTags, Value>> next() = 0;
    };

    virtual ~ValueBlock() = default;

    /**
     * Creates a cursor to iterate over the block.
     *
     * The cursor is guaranteed to be valid during the lifetime of the block. Multiple cursors
     * over the same block are allowed.
     */
    virtual std::unique_ptr<Cursor> cursor() const = 0;

    /**
     * Returns a copy of this block.
     */
    virtual std::unique_ptr<ValueBlock> clone() const = 0;
};

/**
 * A block that contains no values.
 */
struct EmptyBlock final : public ValueBlock {
    struct Cursor : public ValueBlock::Cursor {
        boost::optional<std::pair<value::TypeTags, value::Value>> next() override {
            return boost::none;
        }
    };

    std::unique_ptr<ValueBlock::Cursor> cursor() const override {
        return std::make_unique<Cursor>();
    }

    std::unique_ptr<ValueBlock> clone() const override {
        return std::make_unique<EmptyBlock>();
    }
};
}  // namespace mongo::sbe::value
