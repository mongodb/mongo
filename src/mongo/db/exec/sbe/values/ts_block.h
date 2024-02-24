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
class TsBlock;
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
    std::pair<std::vector<std::unique_ptr<TsBlock>>, std::vector<std::unique_ptr<CellBlock>>>
    extractCellBlocks(const BSONObj& bucket);

private:
    std::vector<CellBlock::PathRequest> _pathReqs;

    // Set of indexes in _pathReqs which are paths NOT of the form [Get <field> Id]. This includes
    // paths like [Get <field> Traverse Id] as well as access to nested paths. Paths of the form
    // [Get <field> Id] can be provided in a special/faster way, since the time series decoding API
    // (currently) provides top-level fields only.
    stdx::unordered_set<size_t> _nonTopLevelGetPathIdxes;


    StringData _timeField;

    // This maps [top-level field -> [index into '_paths' which start with this field]]
    //
    // A vector is needed in case multiple fields with the same prefix (e.g. a.b and a.c) are
    // requested.
    StringDataMap<std::vector<size_t>> _topLevelFieldToIdxes;
};

/**
 * This class implements a block of data in the time series format which is either a BSON object
 * or a binary BSON column. This class is only used for top-level fields.
 */
class TsBlock : public ValueBlock {
public:
    static bool canUseControlValue(value::TypeTags tag) {
        return !isObject(tag) && !isArray(tag);
    }

    // Note: This constructor is special and is only used by the TsCellBlockForTopLevelField to
    // create a TsBlock for a top-level field, where the 'ncells` is actually same as the number of
    // values in this block.
    TsBlock(size_t ncells,
            bool owned,
            TypeTags blockTag,
            Value blockVal,
            int bucketVersion,
            bool isTimefield = false,
            std::pair<TypeTags, Value> controlMin = {TypeTags::Nothing, Value{0u}},
            std::pair<TypeTags, Value> controlMax = {TypeTags::Nothing, Value{0u}});

    // We don't have use cases for copy/move constructors and assignment operators and so disable
    // them until we have one.
    TsBlock(const TsBlock& other) = delete;
    TsBlock(TsBlock&& other) = delete;

    ~TsBlock() override;

    std::unique_ptr<ValueBlock> clone() const override;
    std::unique_ptr<TsBlock> cloneStrongTyped() const;

    DeblockedTagVals deblock(boost::optional<DeblockedTagValStorage>& storage) override;

    // Return whether or not any values of the field are arrays, otherwise return boost::none.
    boost::optional<bool> tryHasNoArrays() {
        if (isArray(_controlMin.first) || isArray(_controlMax.first)) {
            return false;
        } else if (_controlMin.first == _controlMax.first && !isArray(_controlMin.first) &&
                   !isObject(_controlMin.first) && _controlMin.first != TypeTags::Nothing) {
            // Checking !isArray after the initial if statement is redundant but this is the
            // explicit condition we are using to see if a field cannot contain any array values.
            return true;
        }
        return boost::none;
    }

    boost::optional<size_t> tryCount() const override {
        return _count;
    }

    BSONColumn getBSONColumn() const {
        return BSONColumn(BSONBinData{
            value::getBSONBinData(TypeTags::bsonBinData, _blockVal),
            static_cast<int>(value::getBSONBinDataSize(TypeTags::bsonBinData, _blockVal)),
            BinDataType::Column});
    }

    std::pair<TypeTags, Value> tryLowerBound() const override {
        // The time field's control value is rounded down, so we can use it as a lower bound,
        // but cannot necessarily use it as the min().
        if (canUseControlValue(_controlMin.first)) {
            return _controlMin;
        }
        return std::pair{TypeTags::Nothing, Value{0u}};
    }

    std::pair<TypeTags, Value> tryUpperBound() const override {
        return tryMax();
    }

    std::pair<TypeTags, Value> tryMin() const override;

    std::pair<TypeTags, Value> tryMax() const override {
        if (canUseControlValue(_controlMax.first)) {
            return _controlMax;
        }
        return std::pair{TypeTags::Nothing, Value{0u}};
    }

    boost::optional<bool> tryDense() const override {
        return _isTimeField;
    }

private:
    void ensureDeblocked();

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

    bool isTimeFieldSorted() const;

    // TsBlock owned by the TsCellBlockForTopLevelField which in turn is owned by the
    // TsBucketToCellBlockStage can be in a special unowned state of '_blockVal', where it is merely
    // a view on the BSON provided by the stage tree below. This is done as an optimization to avoid
    // copying all the data we read. Any TsBlocks created outside that stage (either via clone() or
    // any other way) are fully owned, and have no pointers to outside data. So, we need to keep
    // track of whether the underlying buffer '_blockVal' is owned or not via '_blockOwned'.
    //
    // If the '_blockVal' is not owned, this TsBlock is valid only as long as the underlying BSON.
    bool _blockOwned;
    TypeTags _blockTag;
    Value _blockVal;

    // The number of values in this block.
    size_t _count;

    // The version of the bucket, which indicates whether the data is compressed and whether the
    // time field is sorted.
    int _bucketVersion;

    // true if all values in the block are non-nothing. Currently only true for timeField
    bool _isTimeField;

    // Store the min and max found in the control field of a bucket
    std::pair<TypeTags, Value> _controlMin;
    std::pair<TypeTags, Value> _controlMax;

    // A HeterogeneousBlock or HomogeneousBlock that stores the decompressed values of the original
    // TsBlock.
    std::unique_ptr<ValueBlock> _decompressedBlock;
};

/**
 * Implements CellBlock interface for timeseries buckets. Currently this class is only used for top
 * level fields. Subfields use a materialized cell block.
 */
class TsCellBlockForTopLevelField : public CellBlock {
public:
    TsCellBlockForTopLevelField(TsBlock* block);

    // We don't have use cases for copy/move constructors and assignment operators and so disable
    // them until we have one.
    TsCellBlockForTopLevelField(const TsCellBlockForTopLevelField& other) = delete;
    TsCellBlockForTopLevelField(TsCellBlockForTopLevelField&&) = delete;
    TsCellBlockForTopLevelField& operator=(const TsCellBlockForTopLevelField& other) = delete;
    TsCellBlockForTopLevelField& operator=(TsCellBlockForTopLevelField&& other) = delete;

    ~TsCellBlockForTopLevelField() override = default;

    ValueBlock& getValueBlock() override;

    std::unique_ptr<CellBlock> clone() const override;

    const std::vector<int32_t>& filterPositionInfo() override {
        return _positionInfo;
    }

private:
    TsCellBlockForTopLevelField(size_t count, std::unique_ptr<TsBlock> tsBlock);

    std::unique_ptr<TsBlock> _ownedTsBlock;
    // If _ownedTsBlock is non-null, this points to _ownedTsBlock.
    TsBlock* _unownedTsBlock;

    // For now this is always empty since only top-level fields are supported.
    std::vector<int32_t> _positionInfo;
};
}  // namespace mongo::sbe::value
