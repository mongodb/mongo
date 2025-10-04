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

#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/bufreader.h"

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace mongo::sbe::value {
class TsBlock;
/**
 * Evaluates paths on a time series bucket. The constructor input is a set of paths using the
 * Get/Traverse/Id primitives. When given a TS bucket, it evaluates each path on the block of
 * "documents" in the TS bucket, producing a block of cells for each path.
 *
 * TODO: For now only top-level fields are supported.
 */
class TsBucketPathExtractor {
public:
    struct ExtractResult {
        size_t numMeasurements = 0;
        std::vector<std::unique_ptr<TsBlock>> storageBlocks;
        std::vector<std::unique_ptr<CellBlock>> cellBlocks;
    };

    TsBucketPathExtractor(std::vector<CellBlock::PathRequest> reqs, StringData timeField);

    /*
     * Returns one CellBlock per path given in the constructor. A CellBlock represents all of the
     * values at a path, along with information on their position.
     */
    ExtractResult extractCellBlocks(const BSONObj& bucket);

private:
    /**
     * Tries to apply path-based decompression to the non-top-level paths for a block.
     *
     * If successful, updates 'outCells' accordingly and returns true. Otherwise, no paths are
     * decompresssed and returns false.
     *
     * Currently path-based decompression is only supported for scalar fields.
     */
    bool tryPathBasedDecompression(TsBlock& tsBlock,
                                   BSONElement fieldMin,
                                   BSONElement fieldMax,
                                   const std::vector<size_t>& nonTopLevelIdxesForCurrentField,
                                   std::vector<std::unique_ptr<CellBlock>>& outCells) const;

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

    std::unique_ptr<ValueBlock> fillEmpty(TypeTags fillTag, Value fillVal) override;

    std::unique_ptr<ValueBlock> fillType(uint32_t typeMask,
                                         TypeTags fillTag,
                                         Value fillVal) override;

    // Returns true if none of the values in this block are arrays or objects. Returns false if
    // any _may_ be arrays or objects.
    bool hasNoObjsOrArrays() const {
        if (_controlMin.first == _controlMax.first && !isArray(_controlMin.first) &&
            !isObject(_controlMin.first) && _controlMin.first != TypeTags::Nothing) {
            // Checking !isArray after the initial if statement is redundant but this is the
            // explicit condition we are using to see if a field cannot contain any array values.
            return true;
        }
        return false;
    }

    size_t count() override {
        return _count;
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
        // Similar to tryLowerBound(), the control can be rounded up. It is a valid upper bound but
        // not a valid max.
        if (canUseControlValue(_controlMax.first)) {
            return _controlMax;
        }
        return std::pair{TypeTags::Nothing, Value{0u}};
    }

    std::pair<TypeTags, Value> tryMin() const override;
    std::pair<TypeTags, Value> tryMax() const override;

    boost::optional<bool> tryDense() const override {
        return _isTimeField;
    }
    boost::optional<bool> tryHasArray() const override;

    // Whether this TS block was decompressed. This is not a method on the block API.
    bool decompressed() const {
        return static_cast<bool>(_decompressedBlock);
    }

    boost::optional<size_t> argMin() override {
        ensureDeblocked();
        return _decompressedBlock->argMin();
    }

    boost::optional<size_t> argMax() override {
        ensureDeblocked();
        return _decompressedBlock->argMax();
    }

    std::pair<value::TypeTags, value::Value> at(size_t idx) override {
        ensureDeblocked();
        return _decompressedBlock->at(idx);
    }

    TypeTags getBlockTag() const {
        return _blockTag;
    }

    /**
     * Returns the BinData for this TsBlock, if present. It's illegal to call this for TsBlocks
     * backed by a bucket which does not use BinData/BSONColumn.
     */
    BSONBinData getBinData() const;

    // Test-only helper.
    ValueBlock* decompressedBlock_forTest() {
        return _decompressedBlock.get();
    }

private:
    TsBlock(bool owned, TypeTags blockTag, Value blockVal);

    void ensureDeblocked();

    /**
     * Deblocks the values from a BSON object block.
     */
    void deblockFromBsonObj();

    /**
     * Deblocks the values from a BSON column block.
     */
    void deblockFromBsonColumn();

    bool isTimeFieldSorted() const;

    // TsBlock owned by the TsCellBlockForTopLevelField which in turn is owned by the
    // TsBucketToCellBlockStage can be in a special unowned state of '_blockVal', where it is merely
    // a view on the BSON provided by the stage tree below. This is done as an optimization to avoid
    // copying all the data we read. Any TsBlocks created outside that stage (either via clone() or
    // any other way) are fully owned, and have no pointers to outside data. So, we need to keep
    // track of whether the underlying buffer '_blockVal' is owned or not via '_blockOwned'.
    //
    // If the '_blockVal' is not owned, this TsBlock is valid only as long as the underlying BSON.
    const bool _blockOwned;
    const TypeTags _blockTag;
    const Value _blockVal;

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
