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

#include "mongo/db/exec/sbe/values/ts_block.h"

#include <cstddef>
#include <memory>
#include <tuple>
#include <utility>

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/bson_block.h"
#include "mongo/db/exec/sbe/values/bsoncolumn_materializer.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/scalar_mono_cell_block.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/util/itoa.h"

namespace mongo::sbe::value {

namespace {
/**
 * Used by the block-based decompressing API to decompress directly into vectors that are needed
 * by other functions to construct blocks. The API requires all 'Containers' to implement
 *'push_back' and 'back'.
 **/
class BlockBasedDecompressAdaptor {
    using Element = sbe::bsoncolumn::SBEColumnMaterializer::Element;
    using ElementStorage = mongo::bsoncolumn::ElementStorage;

public:
    BlockBasedDecompressAdaptor(size_t expectedCount) {
        _tags.reserve(expectedCount);
        _vals.reserve(expectedCount);
    }

    void push_back(const Element& e) {
        auto [tag, val] = e;
        _allValuesShallow = _allValuesShallow && value::isShallowType(tag);
        _tags.push_back(tag);
        _vals.push_back(val);
    }

    Element back() {
        return {_tags.back(), _vals.back()};
    }

    // TODO SERVER-85718 Enable when integrating interleaved mode.
    // void appendPositionInfo(int32_t n) {
    //     _positions.push_back(n);
    // }

    boost::intrusive_ptr<ElementStorage> allocator() const {
        return _allocator;
    }

    std::unique_ptr<value::ValueBlock> extractBlock() {
        if (_allValuesShallow) {
            // We have no deep types, so '_tags' and '_vals' do not contain pointers into
            // '_storage'.  We can pass these into a block type which takes ownership of its
            // values, and we may be able to advantage of homogeneous/repeated data.
            return buildBlockFromStorage(std::move(_tags), std::move(_vals));
        } else {
            // If the block has any deep types, we output an ElementStorageValueBlock to avoid
            // copying the deep value(s).
            return std::make_unique<ElementStorageValueBlock>(
                std::move(_allocator), std::move(_tags), std::move(_vals));
        }
    }

private:
    std::vector<TypeTags> _tags;
    std::vector<Value> _vals;
    // TODO SERVER-85718 Enable when integrating interleaved mode. This vector will hold the
    // position information of the values.
    // std::vector<int32_t> _positions;

    boost::intrusive_ptr<ElementStorage> _allocator{new ElementStorage()};

    // Whether push_back() has been called only with shallow values, which do not point into
    // '_storage'.
    bool _allValuesShallow = true;
};
}  // namespace

TsBucketPathExtractor::TsBucketPathExtractor(std::vector<CellBlock::PathRequest> pathReqs,
                                             StringData timeField)
    : _pathReqs(std::move(pathReqs)),
      _timeField(timeField),
      // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
      _blockBasedDecompressionEnabled(
          feature_flags::gBlockBasedDecodingScalarAPI.isEnabledAndIgnoreFCVUnsafe()) {

    size_t idx = 0;
    for (auto& req : _pathReqs) {
        tassert(7796405,
                "Path must start with a Get operation",
                holds_alternative<CellBlock::Get>(req.path[0]));

        StringData field = get<CellBlock::Get>(req.path[0]).field;
        _topLevelFieldToIdxes[field].push_back(idx);

        if (req.path.size() > 2) {
            _nonTopLevelGetPathIdxes.insert(idx);
        }

        ++idx;
    }
}

TsBucketPathExtractor::ExtractResult TsBucketPathExtractor::extractCellBlocks(
    const BSONObj& bucketObj) {

    BSONElement bucketControl = bucketObj[timeseries::kBucketControlFieldName];
    invariant(!bucketControl.eoo());

    const size_t noOfMeasurements = [&]() {
        if (auto ct = bucketControl.Obj()[timeseries::kBucketControlCountFieldName]) {
            return static_cast<size_t>(ct.numberLong());
        }
        return static_cast<size_t>(
            timeseries::BucketUnpacker::computeMeasurementCount(bucketObj, StringData(_timeField)));
    }();

    const int bucketVersion = bucketObj.getIntField(timeseries::kBucketControlVersionFieldName);

    const BSONElement bucketDataElem = bucketObj[timeseries::kBucketDataFieldName];
    invariant(!bucketDataElem.eoo());
    invariant(bucketDataElem.type() == BSONType::Object);

    // Build a mapping from the top level field name to the bucket's corresponding bson element.
    StringMap<BSONElement> topLevelFieldToBsonElt;
    for (auto elt : bucketDataElem.embeddedObject()) {
        auto it = _topLevelFieldToIdxes.find(elt.fieldNameStringData());
        if (it != _topLevelFieldToIdxes.end()) {
            auto [blockTag, blockVal] = bson::convertFrom<true>(elt);
            tassert(7796400,
                    "Unsupported type for timeseries bucket data",
                    blockTag == value::TypeTags::bsonObject ||
                        blockTag == value::TypeTags::bsonBinData);
            topLevelFieldToBsonElt[elt.fieldName()] = elt;
        }
    }

    std::vector<std::unique_ptr<TsBlock>> outBlocks;
    std::vector<std::unique_ptr<CellBlock>> outCells(_pathReqs.size());

    // The time series decoding API gives us the top level fields only. To simulate an API
    // which lets us extract more granular paths, we materialize each top level field as BSON,
    // and then extract from that. This is awful in terms of performance, but it can be swapped
    // out with a more efficient implementation when a more granular API becomes available.

    auto bucketControlMin = bucketControl.Obj()[timeseries::kBucketControlMinFieldName];
    auto bucketControlMax = bucketControl.Obj()[timeseries::kBucketControlMaxFieldName];

    for (auto& [topLevelField, columnElt] : topLevelFieldToBsonElt) {
        // The set of indexes in _pathReqs which begin with this top level field.
        const auto& pathIndexesForCurrentField = _topLevelFieldToIdxes[topLevelField];
        auto [columnTag, columnVal] = bson::convertFrom<true /*View*/>(columnElt);

        std::pair<TypeTags, Value> controlMin = {TypeTags::Nothing, Value{0u}};
        std::pair<TypeTags, Value> controlMax = {TypeTags::Nothing, Value{0u}};
        if (!bucketControlMin.eoo() && !bucketControlMax.eoo()) {
            auto fieldMin = bucketControlMin[topLevelField];
            controlMin = bson::convertFrom<true /*View*/>(fieldMin);

            auto fieldMax = bucketControlMax[topLevelField];
            controlMax = bson::convertFrom<true /*View*/>(fieldMax);
        }

        // The time field cannot be nothing.
        const bool isTimeField = topLevelField == _timeField;

        // Initialize a TsBlock for the top level field. For paths of the form [Get <field> Id], or
        // equivalent, we will create a CellBlock. For nested paths that aren't eligible for the
        // fast path, we will call extract() on this TsBlock, and then pull the values out from the
        // nested bson.
        outBlocks.push_back(std::make_unique<TsBlock>(noOfMeasurements,
                                                      false /*owned*/,
                                                      columnTag,
                                                      columnVal,
                                                      bucketVersion,
                                                      isTimeField,
                                                      _blockBasedDecompressionEnabled,
                                                      controlMin,
                                                      controlMax));
        auto tsBlock = outBlocks.back().get();

        // Build a list of values 'pathIndexesForCurrentField' which are not top-level Gets.
        std::vector<size_t> nonTopLevelIdxesForCurrentField;

        for (auto idx : pathIndexesForCurrentField) {
            if (_nonTopLevelGetPathIdxes.count(idx) == 0) {
                // This path is a top level [Get <field> Id] path. We assign to its corresponding
                // output the top level cellblock. Note that we keep the raw pointer to this
                // CellBlock in 'topLevelCellBlock' so that if we end up decoding this CellBlock,
                // we do so once, and via same TsCellBlockForTopLevelField instance.
                outCells[idx] = std::make_unique<TsCellBlockForTopLevelField>(tsBlock);
            } else {
                // Remember this PathReq index for later.
                nonTopLevelIdxesForCurrentField.push_back(idx);
            }
        }

        // There are no more paths that were requested which begin with this top level field.
        if (nonTopLevelIdxesForCurrentField.empty()) {
            continue;
        }

        // First check if we are traversing a top level field AND there are no arrays. The path
        // must look like: [Get <field> Traverse Id]. If this is the case, we take a fast path and
        // skip the work of shredding the whole thing.

        bool allUsedFastPath = true;
        for (auto pathIdx : nonTopLevelIdxesForCurrentField) {
            if (_pathReqs[pathIdx].path.size() == 3 &&
                holds_alternative<CellBlock::Get>(_pathReqs[pathIdx].path[0]) &&
                holds_alternative<CellBlock::Traverse>(_pathReqs[pathIdx].path[1]) &&
                holds_alternative<CellBlock::Id>(_pathReqs[pathIdx].path[2]) &&
                (tsBlock->hasNoObjsOrArrays())) {
                // In this case the top level TsCellBlockForTopLevelField (representing the [Get
                // <field> Id]) is identical to the path [Get <field> Traverse Id]. We make a top
                // level cell block with an unowned pointer.
                outCells[pathIdx] = std::make_unique<TsCellBlockForTopLevelField>(tsBlock);
            } else {
                allUsedFastPath = false;
            }
        }

        if (allUsedFastPath) {
            // There's no need to do any more work for this top level field. Every path request
            // was a top level get or eligible for the fast path.
            continue;
        }

        // This is the slow path. We materialize the top level field into BSON and then re-read
        // that BSON to produce the results for nested paths.

        auto extracted = tsBlock->extract();
        invariant(extracted.count() == noOfMeasurements);

        std::vector<CellBlock::PathRequest> reqs;
        for (auto idx : nonTopLevelIdxesForCurrentField) {
            reqs.push_back(_pathReqs[idx]);
        }

        auto extractor = value::BSONCellExtractor::make(reqs);
        auto extractedCellBlocks = extractor->extractFromTopLevelField(
            topLevelField, extracted.tagsSpan(), extracted.valsSpan());
        invariant(reqs.size() == extractedCellBlocks.size());

        for (size_t i = 0; i < extractedCellBlocks.size(); ++i) {
            outCells[nonTopLevelIdxesForCurrentField[i]] = std::move(extractedCellBlocks[i]);
        }
    }

    // Fill in any empty spots in the output with a block of [Nothing, Nothing...].
    for (auto& cellBlock : outCells) {
        if (!cellBlock) {
            auto emptyBlock = std::make_unique<value::ScalarMonoCellBlock>(
                noOfMeasurements, value::TypeTags::Nothing, value::Value(0));
            cellBlock = std::move(emptyBlock);
        }
    }
    return ExtractResult{noOfMeasurements, std::move(outBlocks), std::move(outCells)};
}

TsBlock::TsBlock(size_t ncells,
                 bool owned,
                 TypeTags blockTag,
                 Value blockVal,
                 int bucketVersion,
                 bool isTimeField,
                 bool blockBasedDecompressionEnabled,
                 std::pair<TypeTags, Value> controlMin,
                 std::pair<TypeTags, Value> controlMax)
    : _blockOwned(owned),
      _blockTag(blockTag),
      _blockVal(blockVal),
      _count(ncells),
      _bucketVersion(bucketVersion),
      _isTimeField(isTimeField),
      _blockBasedDecompressionEnabled(blockBasedDecompressionEnabled),
      _controlMin(copyValue(controlMin.first, controlMin.second)),
      _controlMax(copyValue(controlMax.first, controlMax.second)) {
    invariant(_blockTag == TypeTags::bsonObject || _blockTag == TypeTags::bsonBinData);
}

TsBlock::~TsBlock() {
    if (_blockOwned) {
        // The underlying buffer is owned by this TsBlock and so this releases it.
        releaseValue(_blockTag, _blockVal);
    }
    // controlMin and controlMax are always owned so we always need to release them.
    releaseValue(_controlMin.first, _controlMin.second);
    releaseValue(_controlMax.first, _controlMax.second);
}

void TsBlock::deblockFromBsonObj() {
    std::vector<TypeTags> tags;
    std::vector<Value> vals;
    tags.reserve(_count);
    vals.reserve(_count);

    ValueVectorGuard vectorGuard(tags, vals);

    ObjectEnumerator enumerator(TypeTags::bsonObject, _blockVal);
    for (size_t i = 0; i < _count; ++i) {
        auto [tag, val] = [&] {
            if (enumerator.atEnd() || ItoA(i) != enumerator.getFieldName()) {
                // There's a missing index or a hole in the middle or at the tail, so returns
                // Nothing.
                return std::make_pair(TypeTags::Nothing, Value(0));
            } else {
                auto tagVal = enumerator.getViewOfValue();
                enumerator.advance();
                // Always makes a copy to match the behavior to the BSONColumn case's and simplify
                // the SBE value ownership model. The underlying buffer for the BSON object block is
                // owned by this TsBlock or not so we would not necessarily need to always copy the
                // values out of it.
                //
                // TODO SERVER-79612: Avoid copying values out of the BSON object block if
                // necessary.
                return copyValue(tagVal.first, tagVal.second);
            }
        }();

        tags.push_back(tag);
        vals.push_back(val);
    }

    vectorGuard.reset();
    _decompressedBlock = buildBlockFromStorage(std::move(tags), std::move(vals));
}

void TsBlock::deblockFromBsonColumn() {
    const auto binData = getBinData();

    // If we can guarantee there are no arrays nor objects in this column, and the feature flag is
    // enabled, use the faster block-based decoding API.
    if (_blockBasedDecompressionEnabled && hasNoObjsOrArrays()) {
        using SBEMaterializer = sbe::bsoncolumn::SBEColumnMaterializer;
        mongo::bsoncolumn::BSONColumnBlockBased col(binData);

        BlockBasedDecompressAdaptor container(_count);
        col.decompress<SBEMaterializer, BlockBasedDecompressAdaptor>(container,
                                                                     container.allocator());
        _decompressedBlock = container.extractBlock();
    } else {
        // Use the old, less efficient decoder, if there may be objects or arrays.
        BSONColumn blockColumn(binData);
        auto it = blockColumn.begin();

        std::vector<TypeTags> tags;
        std::vector<Value> vals;
        tags.reserve(_count);
        vals.reserve(_count);

        // Generally when we're in this path, we expect to be decompressing deep types. Instead of
        // copying the values into an owned value block, we insert them into an
        // ElementStorageValueBlock which will keep the BSONColumn's ElementStorage around. This
        // prevents us from using HomogeneousBlock, but lets us avoid the copy.

        for (size_t i = 0; i < _count; ++i) {
            auto [tag, val] = bson::convertFrom</*View*/ true>(*it);

            // No copy.

            ++it;
            tags.push_back(tag);
            vals.push_back(val);
        }

        // Preserve the storage for this BSONColumn so it lives as long as this TsBlock is alive.
        _decompressedBlock = std::make_unique<ElementStorageValueBlock>(
            blockColumn.release(), std::move(tags), std::move(vals));
    }
}

std::unique_ptr<TsBlock> TsBlock::cloneStrongTyped() const {
    // TODO: If we've already decoded the output, there's no need to re-copy the entire bson
    // column. We could instead just copy the decoded values and metadata.

    auto [cpyTag, cpyVal] = copyValue(_blockTag, _blockVal);
    ValueGuard guard(cpyTag, cpyVal);
    // The new copy must own the copied underlying buffer.
    auto cpy = std::make_unique<TsBlock>(_count,
                                         /*owned*/ true,
                                         cpyTag,
                                         cpyVal,
                                         _bucketVersion,
                                         _isTimeField,
                                         _blockBasedDecompressionEnabled);
    guard.reset();

    // TODO: This might not be necessary now that TsBlock doesn't really use _deblockedStorage
    // If the block has been deblocked, then we need to copy the deblocked values too to
    // avoid deblocking overhead again.
    cpy->_deblockedStorage = _deblockedStorage;

    // If the block has been decompressed into a HeterogenousBlock or Homogeneous copy, we need to
    // copy this decompressed block.
    if (_decompressedBlock) {
        cpy->_decompressedBlock = _decompressedBlock->clone();
    }

    return cpy;
}

std::unique_ptr<ValueBlock> TsBlock::clone() const {
    return std::unique_ptr<ValueBlock>(cloneStrongTyped().release());
}

DeblockedTagVals TsBlock::deblock(boost::optional<DeblockedTagValStorage>& storage) {
    ensureDeblocked();

    return _decompressedBlock->extract();
}

BSONBinData TsBlock::getBinData() const {
    tassert(7796401,
            "Invalid BinDataType for BSONColumn",
            getBSONBinDataSubtype(TypeTags::bsonBinData, _blockVal) == BinDataType::Column);

    return BSONBinData{
        value::getBSONBinData(TypeTags::bsonBinData, _blockVal),
        static_cast<int>(value::getBSONBinDataSize(TypeTags::bsonBinData, _blockVal)),
        BinDataType::Column};
}

std::pair<TypeTags, Value> TsBlock::tryMin() const {
    // V1 and v3 buckets store the time field unsorted. In all versions, the control.min of the time
    // field is rounded down. If computing the true minimum requires traversing the whole column, we
    // just return Nothing.
    if (_isTimeField) {
        if (isTimeFieldSorted()) {
            // control.min is only a lower bound for the time field but v2 buckets are sorted by
            // time, so we can easily get the true min by reading the first element in the block.
            auto blockColumn = BSONColumn(getBinData());
            auto it = blockColumn.begin();
            auto [trueMinTag, trueMinVal] = bson::convertFrom</*View*/ true>(*it);
            return value::copyValue(trueMinTag, trueMinVal);
        }
    } else if (canUseControlValue(_controlMin.first)) {
        return _controlMin;
    }

    return std::pair{TypeTags::Nothing, Value{0u}};
}

void TsBlock::ensureDeblocked() {
    if (!_decompressedBlock) {
        if (_blockTag == TypeTags::bsonObject) {
            deblockFromBsonObj();
        } else {
            deblockFromBsonColumn();
        }
        tassert(
            8867300, "Decompressed block must be set after ensureDeblocked()", _decompressedBlock);
    }
}

bool TsBlock::isTimeFieldSorted() const {
    return _bucketVersion == timeseries::kTimeseriesControlCompressedSortedVersion;
}

boost::optional<bool> TsBlock::tryHasArray() const {
    if (hasNoObjsOrArrays()) {
        return false;
    }
    if (isArray(_controlMin.first) || isArray(_controlMax.first)) {
        return true;
    }
    return boost::none;
}

std::unique_ptr<ValueBlock> TsBlock::fillEmpty(TypeTags fillTag, Value fillVal) {
    ensureDeblocked();
    return _decompressedBlock->fillEmpty(fillTag, fillVal);
}

std::unique_ptr<ValueBlock> TsBlock::fillType(uint32_t typeMask, TypeTags fillTag, Value fillVal) {
    if (static_cast<bool>(getBSONTypeMask(_controlMin.first) & kNumberMask) &&
        static_cast<bool>(getBSONTypeMask(_controlMax.first) & kNumberMask) &&
        !static_cast<bool>(kNumberMask & typeMask)) {
        // The control min and max tags are both numbers, and the target typeMask doesn't cover
        // numbers which are in the same canonical type bracket (see canonicalizeBSONType()). Even
        // though the block could have Nothings, fillType on Nothing always returns Nothing.
        return nullptr;
    } else if (static_cast<bool>(getBSONTypeMask(_controlMin.first) & kDateMask) &&
               _controlMin.first == _controlMax.first && !static_cast<bool>(kDateMask & typeMask)) {
        // The control min and max tags are both dates and the target typeMask doesn't cover Dates.
        // Since Dates are in their own canonical type bracket (see canonicalizeBSONType()) and
        // fillType on Nothing returns Nothing, we can return the block unchanged without having to
        // extract.
        return nullptr;
    }

    return ValueBlock::fillType(typeMask, fillTag, fillVal);
}

ValueBlock& TsCellBlockForTopLevelField::getValueBlock() {
    return *_unownedTsBlock;
}

std::unique_ptr<CellBlock> TsCellBlockForTopLevelField::clone() const {
    auto precomputedCount = _unownedTsBlock->tryCount();
    tassert(
        7943900, "Assumes count() is available in O(1) time on TS Block type", precomputedCount);
    auto tsBlockClone = _unownedTsBlock->cloneStrongTyped();

    // Using raw new to access private constructor.
    return std::unique_ptr<TsCellBlockForTopLevelField>(
        new TsCellBlockForTopLevelField(*precomputedCount, std::move(tsBlockClone)));
}

TsCellBlockForTopLevelField::TsCellBlockForTopLevelField(TsBlock* block) : _unownedTsBlock(block) {
    auto count = block->tryCount();
    tassert(8182400, "Assumes count() is available in O(1) time on TS Block type", count);
    // Position info of 1111...
    _positionInfo.resize(*count, 1);
}

TsCellBlockForTopLevelField::TsCellBlockForTopLevelField(size_t count,
                                                         std::unique_ptr<TsBlock> tsBlock)
    : _ownedTsBlock(std::move(tsBlock)), _unownedTsBlock(_ownedTsBlock.get()) {
    // Position info of 1111...
    _positionInfo.resize(count, 1);
}
}  // namespace mongo::sbe::value
