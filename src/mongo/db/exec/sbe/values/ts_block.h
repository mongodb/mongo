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

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/bufreader.h"

namespace mongo::sbe::value {
/**
 * Evaluates paths on a time series bucket. The constructor input is a set of paths using the
 * Get/Traverse/Id primitives. When given a TS bucket, it evaluates each path on the block of
 * "documents" in the TS bucket, producing a block of cells for each path.
 *
 * TODO PM-3402/after PM-3402: Swap out the naive implementation here with one that uses the new
 * decoding API.
 *
 * TODO: For now only top-level fields are supported.
 */
class TsBucketPathExtractor {
public:
    TsBucketPathExtractor(std::vector<CellBlock::PathRequest> reqs, StringData timeField);

    /*
     * Returns one CellBlock per path given in the constructor. A CellBlock represents all of the
     * values at a path, along with information on their position.
     */
    std::vector<std::unique_ptr<CellBlock>> extractCellBlocks(const BSONObj& bucket);

private:
    std::vector<CellBlock::PathRequest> _pathReqs;

    // Logically a pair of [path, idx] for the paths that are not top level. Each path appears in
    // '_paths' vector.
    std::vector<CellBlock::PathRequest> _nonTopLevelPathReqs;
    std::vector<size_t> _nonTopLevelPathIdxes;


    StringData _timeField;

    // This maps [top-level field -> [index into '_paths' which start with this field]]
    //
    // A vector is needed in case multiple fields with the same prefix (e.g. a.b and a.c) are
    // requested.
    StringDataMap<std::vector<size_t>> _topLevelFieldToIdxes;

    // The top level fields which have subsequent subfield access.
    StringSet _topLevelFieldsWithSubfieldAccess;
};

/**
 * This class implements a block of data in the time series format which is either a BSON object
 * or a binary BSON column. This class is only used for top-level fields.
 */
class TsBlock : public ValueBlock {
public:
    // Note: This constructor is special and is only used by the TsCellBlock to create a TsBlock for
    // a top-level field, where the 'ncells` is actually same as the number of values in this block.
    TsBlock(size_t ncells, bool owned, TypeTags blockTag, Value blockVal);

    // We don't have use cases for copy/move constructors and assignment operators and so disable
    // them until we have one.
    TsBlock(const TsBlock& other) = delete;
    TsBlock(TsBlock&& other) = delete;

    ~TsBlock() override;

    std::unique_ptr<ValueBlock> clone() const override;

    DeblockedTagVals deblock(boost::optional<DeblockedTagValStorage>& storage) const override {
        ensureDeblocked(storage);

        return DeblockedTagVals{storage->vals.size(), storage->tags.data(), storage->vals.data()};
    }

    boost::optional<size_t> tryCount() const override {
        return _count;
    }

private:
    void ensureDeblocked(boost::optional<DeblockedTagValStorage>& storage) const {
        if (!storage) {
            storage = DeblockedTagValStorage{};

            storage->owned = true;
            storage->tags.reserve(_count);
            storage->vals.reserve(_count);

            if (_blockTag == TypeTags::bsonObject) {
                deblockFromBsonObj(storage->tags, storage->vals);
            } else {
                deblockFromBsonColumn(storage->tags, storage->vals);
            }
        }
    }

    /**
     * Deblocks the values from a BSON object block.
     */
    void deblockFromBsonObj(std::vector<TypeTags>& deblockedTags,
                            std::vector<Value>& deblockedVals) const;

    /**
     * Deblocks the values from a BSON column block.
     */
    void deblockFromBsonColumn(std::vector<TypeTags>& deblockedTags,
                               std::vector<Value>& deblockedVals) const;

    // TsBlock owned by the TsCellBlock which in turn is owned by the TsBucketToCellBlockStage can
    // be in a special unowned state of '_blockVal', where it is merely a view on the BSON provided
    // by the stage tree below. This is done as an optimization to avoid copying all the data we
    // read. Any TsBlocks created outside that stage (either via clone() or any other way) are fully
    // owned, and have no pointers to outside data. So, we need to keep track of whether the
    // underlying buffer '_blockVal' is owned or not via '_blockOwned'.
    //
    // If the '_blockVal' is not owned, this TsBlock is valid only as long as the underlying BSON.
    bool _blockOwned;
    TypeTags _blockTag;
    Value _blockVal;

    // The number of values in this block.
    size_t _count;
};

/**
 * Implements CellBlock interface for timeseries buckets. Currently this class is only used for top
 * level fields. Subfields use a materialized cell block.
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
    TsCellBlock(size_t count, bool owned, TypeTags topLevelTag, Value topLevelVal);

    // We don't have use cases for copy/move constructors and assignment operators and so disable
    // them until we have one.
    TsCellBlock(const TsCellBlock& other) = delete;
    TsCellBlock(TsCellBlock&&) = delete;
    TsCellBlock& operator=(const TsCellBlock& other) = delete;
    TsCellBlock& operator=(TsCellBlock&& other) = delete;

    ~TsCellBlock() override = default;

    ValueBlock& getValueBlock() override;

    std::unique_ptr<CellBlock> clone() const override;

    const std::vector<char>& filterPositionInfo() override {
        return _positionInfo;
    }

private:
    TypeTags _blockTag = TypeTags::Nothing;
    Value _blockVal = Value(0);

    TsBlock _tsBlock;

    // For now this is always empty since only top-level fields are supported.
    std::vector<char> _positionInfo;
};
}  // namespace mongo::sbe::value
