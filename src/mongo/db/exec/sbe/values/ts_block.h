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
#include <type_traits>

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/bufreader.h"

namespace mongo::sbe::value {
namespace {
template <typename T>
requires std::is_base_of_v<ValueBlock, T> || std::is_base_of_v<CellBlock, T>
auto makeDeepCopy(TypeTags blockTag, Value blockVal) -> std::unique_ptr<T> {
    // Makes a deep copy.
    auto [blockCopyTag, blockCopyVal] = copyValue(blockTag, blockVal);
    ValueGuard guard{blockCopyTag, blockCopyVal};
    // The new block should own the underlying storage buffer since we made a deep copy of it.
    auto blockCopy = std::make_unique<T>(/*owned*/ true, blockCopyTag, blockCopyVal);
    guard.reset();
    return blockCopy;
}
}  // namespace

/**
 * This class implements a block of data in the time series format which is either a BSON object
 * or a binary BSON column.
 *
 * Note: If the underlying storage buffer is not owned by this block, which is marked by '_owned' ==
 * false, the caller is responsible to make it alive during the lifetime of this block.
 */
class TsBlock : public ValueBlock {
public:
    TsBlock(bool owned, value::TypeTags blockTag, value::Value blockVal);
    // By default, the copy constructor makes a shallow copy of this block. For a deep copy, use the
    // clone() method.
    TsBlock(const TsBlock& other)
        : _owned(false), _blockTag(other._blockTag), _blockVal(other._blockVal) {}
    TsBlock(TsBlock&& other) = default;

    // TODO SERVER-78887: Implement copy/move assignment operators if necessary.
    TsBlock& operator=(const TsBlock& other) = delete;
    TsBlock& operator=(TsBlock&& other) = delete;

    ~TsBlock() override;

    /**
     * In BSONObj-based TS bucket, the values for each block are stored as objects that use numeric
     * strings for the field names, e.g. {"0": 7, "1": 11, "3": 5, "25": 0}. The field names are
     * non-negative integers that are monotonically increasing in numeric order but might have gaps.
     *
     * To be able to syncronize values across multiple blocks in the same TS bucket, the cursor will
     * return 'Nothing' when encountering a gap. Notice, that the cursor doesn't have a notion of a
     * gap at the end of the block -- the client is responsible to handle this situation on their
     * side.
     *
     * Used when the TS block is a BSONObj.
     */
    class Cursor : public ValueBlock::Cursor {
    public:
        Cursor(Value blockVal) : _enumerator(TypeTags::bsonObject, blockVal) {}

        boost::optional<std::pair<TypeTags, Value>> next() override;

    private:
        value::ObjectEnumerator _enumerator;
        // The requested index for a value in this block. There could be hole(s) in the data array
        // index, for example, {"0": ..., "2": ...}. To avoid blindly returning data at the index
        // that '_enumerator' points to, we need to keep track of the requested index. The
        // '_reqIndex' is incremented per each call to next() until the end of '_enumerator'.
        int _reqIndex = 0;
    };

    // Used when the TS block is a binary BSONColumn
    class BsonColumnCursor : public ValueBlock::Cursor {
    public:
        BsonColumnCursor(Value blockVal)
            : _bsonColumn(BSONBinData{
                  value::getBSONBinData(TypeTags::bsonBinData, blockVal),
                  static_cast<int>(value::getBSONBinDataSize(TypeTags::bsonBinData, blockVal)),
                  BinDataType::Column}),
              _it(_bsonColumn.begin()) {}

        boost::optional<std::pair<TypeTags, Value>> next() override;

    private:
        BSONColumn _bsonColumn;
        BSONColumn::Iterator _it;
        // BSONColumn::Iterator decompresses the data on the fly into its own temporary buffer which
        // is invalidated whenever operator++ is called and so we can't return value until we
        // advance the iterator. But '_it' is initialized to the first value and so we should not
        // advance the '_it' on the first call to this method and we need to differentiate the
        // second or later calls from the first call.
        bool _alreadyCalledBefore = false;
        TypeTags _curTag = value::TypeTags::Nothing;
        Value _curVal = 0;
    };

    /**
     * Creates different Cursor implementation, depending on the underlying storage
     */
    std::unique_ptr<ValueBlock::Cursor> cursor() const override {
        if (_blockTag == value::TypeTags::bsonObject) {
            return std::make_unique<Cursor>(_blockVal);
        } else {
            tassert(7796401,
                    "Invalid BinDataType for BSONColumn",
                    getBSONBinDataSubtype(_blockTag, _blockVal) == BinDataType::Column);
            return std::make_unique<BsonColumnCursor>(_blockVal);
        }
    }

    std::unique_ptr<ValueBlock> clone() const override {
        // Makes a deep copy of this block.
        return makeDeepCopy<TsBlock>(_blockTag, _blockVal);
    }

private:
    // True if this block owns the underlying storage buffer which is pointed to by '_blockVal'.
    bool _owned;
    value::TypeTags _blockTag;
    value::Value _blockVal;
};

/**
 * Implements CellBlock interface for timeseries buckets.
 *
 * Note: If the underlying storage buffer is not owned by this block, which is marked by '_owned' ==
 * false, the caller is responsible to make it alive during the lifetime of this block.
 */
class TsCellBlock : public CellBlock {
public:
    /**
     * Constructor.
     *
     * Note: The topLevel in 'topLevel*' parameters means that the value is not nested one inside
     * sub-field of TS bucket "data" field. For example, in the following TS bucket "data" field:
     * {
     *   "control": {...},
     *   "data": {
     *     "foo": {"0": {"a": 1, "b": 1}, "1": [{"a": 2, "b": 2}, {"a": 3, "b": 3}]},
     *   }
     * }
     * the 'topLevelTag' and 'topLevelVal' must be for the value of path "foo" field (hence the
     * top-level), not for the value of paths "foo.a" or "foo.b". The top-level path does not
     * require path navigation.
     */
    TsCellBlock(bool owned, value::TypeTags topLevelTag, value::Value topLevelVal);
    // By default, the copy constructor makes a shallow copy of this block. For a deep copy, use the
    // clone() method.
    TsCellBlock(const TsCellBlock& other)
        : _owned(false), _blockTag(other._blockTag), _blockVal(other._blockVal) {}
    TsCellBlock(TsCellBlock&&) = default;

    // TODO SERVER-78887: Implement copy/move assignment operators if necessary.
    TsCellBlock& operator=(const TsCellBlock& other) = delete;
    TsCellBlock& operator=(TsCellBlock&& other) = delete;

    ~TsCellBlock() override;

    const ValueBlock& getValueBlock() const override;

    std::unique_ptr<CellBlock> clone() const override {
        // Makes a deep copy of this block.
        return makeDeepCopy<TsCellBlock>(_blockTag, _blockVal);
    }

private:
    void ensureValueBlockExists() const;

    // True if this block owns the underlying storage buffer which is pointed to by '_blockVal'.
    bool _owned;
    value::TypeTags _blockTag;
    value::Value _blockVal;

    // Cached
    mutable boost::optional<TsBlock> _valueBlock;
};
}  // namespace mongo::sbe::value
