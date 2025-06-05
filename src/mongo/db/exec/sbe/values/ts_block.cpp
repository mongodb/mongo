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

#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/util/bsonobj_traversal.h"
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

#include <cstddef>
#include <memory>
#include <tuple>
#include <utility>

namespace mongo::sbe::value {

namespace {
/**
 * Used by the block-based decompressing API to decompress directly into vectors that are needed
 * by other functions to construct blocks. The API requires all 'Containers' to implement
 *'push_back' and 'back'.
 **/
class BlockBasedDecompressAdaptor {
    using Element = sbe::bsoncolumn::SBEColumnMaterializer::Element;

public:
    BlockBasedDecompressAdaptor(
        size_t expectedCount,
        boost::intrusive_ptr<BSONElementStorage> allocator = new BSONElementStorage{})
        : _allocator(allocator) {
        _tags.reserve(expectedCount);
        _vals.reserve(expectedCount);
        _positions.reserve(expectedCount);
    }

    MONGO_COMPILER_ALWAYS_INLINE_GCC14 void push_back(const Element& e) {
        auto [tag, val] = e;
        _allValuesShallow = _allValuesShallow && value::isShallowType(tag);
        _tags.push_back(tag);
        _vals.push_back(val);
    }

    Element back() {
        return {_tags.back(), _vals.back()};
    }

    void appendPositionInfo(int32_t n) {
        _positions.push_back(n);
    }

    boost::intrusive_ptr<BSONElementStorage> allocator() const {
        return _allocator;
    }

    std::unique_ptr<value::ValueBlock> extractBlock() {
        if (_allValuesShallow) {
            // We have no deep types, so '_tags' and '_vals' do not contain pointers into
            // '_storage'.  We can pass these into a block type which takes ownership of its
            // values, and we may be able to advantage of homogeneous/repeated data.
            return buildBlockFromStorage(std::move(_tags), std::move(_vals));
        } else {
            // If the block has any deep types, we output an BSONElementStorageValueBlockk to avoid
            // copying the deep value(s).
            return std::make_unique<BSONElementStorageValueBlock>(
                std::move(_allocator), std::move(_tags), std::move(_vals));
        }
    }

    std::vector<int32_t> extractPositionInfo() {
        return std::move(_positions);
    }

private:
    std::vector<TypeTags> _tags;
    std::vector<Value> _vals;
    std::vector<int32_t> _positions;

    boost::intrusive_ptr<BSONElementStorage> _allocator;

    // Whether push_back() has been called only with shallow values, which do not point into
    // '_storage'.
    bool _allValuesShallow = true;
};

struct ElementAnalysis {
    // The set of all scalar values reachable along an arbitrarily deep path containing objects only
    // (no arrays).
    absl::flat_hash_set<const char*> _scalarValuesInObjects = {};
    bool _containsArrays = false;
};

/**
 * Recursively traverses 'elem' for any scalar fields, updating the analysis to reflect presence of
 * arrays and pointers to scalar values.
 */
void analyzeElement(ElementAnalysis& analysis, BSONElement elem) {
    if (elem.type() == BSONType::array) {
        analysis._containsArrays = true;
        // Do not recurse into children; elements within arrays are not candidates for path-based
        // decompression due to challenges with arrays in BSONColumns with legacy interleaved mode.
        // See SERVER-90712.
        return;
    }

    // Note: isABSONObj() returns true for both objects and arrays. But we just checked that this is
    // not an array.
    if (!elem.isABSONObj()) {
        analysis._scalarValuesInObjects.insert(elem.value());
        return;
    }

    for (auto e : elem.embeddedObject()) {
        analyzeElement(analysis, e);
    }
}

/**
 * If the fast scalar-in-object optimization can be applied for the given PathRequest, return the
 * bsoncolumn::SBEPath that corresponds to it. Otherwise, return none.
 *
 * The optimization can be applied if:
 * - The path is a filter path (not a project path)
 * - If the path selects nothing, or if it selects exactly one scalar field for both the min and max
 *   in the column. In practice we probably only check one of the min or max, but both are checked
 *   here out of an abundance of caution.
 */
boost::optional<bsoncolumn::SBEPath> canUsePathBasedDecompression(
    const CellBlock::PathRequest& path,
    const ElementAnalysis& analysis,
    BSONElement elemMin,
    BSONElement elemMax) {

    // We can only handle filter paths if there are arrays in the object.
    if (path.type != CellBlock::kFilter && analysis._containsArrays) {
        return boost::none;
    }

    // Trim the Get in the 0-th element.
    invariant(holds_alternative<CellBlock::Get>(path.path[0]));
    size_t offset = 1;
    if (holds_alternative<CellBlock::Traverse>(path.path[1])) {
        // Also skip over a Traverse element if one is there.
        offset = 2;
    }

    auto trimmed = std::span{path.path}.subspan(offset);
    invariant(trimmed.size() >= 1);
    if (trimmed.size() == 1) {
        invariant(holds_alternative<CellBlock::Id>(trimmed[0]));
        // This is an object and the path is just "Id", so not a scalar.
        return boost::none;
    }

    CellBlock::PathRequest trimmedPath = CellBlock::PathRequest{path.type};
    trimmedPath.path.assign(trimmed.begin(), trimmed.end());
    bsoncolumn::SBEPath sbePath{trimmedPath};

    // If the path matches nothing, or a single scalar element, then we can apply the optimization.
    //
    // TODO SERVER-89514 Refactor this to avoid traversing both fieldMin and fieldMax for every
    // path.
    for (auto elem : {elemMin, elemMax}) {
        auto elems = sbePath.elementsToMaterialize(elem.Obj());
        if (elems.size() > 1) {
            return boost::none;
        }

        if (!elems.empty() && !analysis._scalarValuesInObjects.contains(elems[0])) {
            return boost::none;
        }
    }

    return sbePath;
}

}  // namespace

TsBucketPathExtractor::TsBucketPathExtractor(std::vector<CellBlock::PathRequest> pathReqs,
                                             StringData timeField)
    : _pathReqs(std::move(pathReqs)), _timeField(timeField) {

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
    invariant(bucketControl.type() == BSONType::object);
    BSONObj bucketControlObj = bucketControl.Obj();

    const size_t noOfMeasurements = [&]() {
        if (auto ct = bucketControlObj[timeseries::kBucketControlCountFieldName]) {
            return static_cast<size_t>(ct.numberLong());
        }
        return static_cast<size_t>(
            timeseries::BucketUnpacker::computeMeasurementCount(bucketObj, StringData(_timeField)));
    }();

    const int bucketVersion = bucketObj.getIntField(timeseries::kBucketControlVersionFieldName);

    const BSONElement bucketDataElem = bucketObj[timeseries::kBucketDataFieldName];
    invariant(bucketDataElem.type() == BSONType::object);

    // Build a mapping from the top level field name to the bucket's corresponding bson element and
    // min/max values.
    struct FieldInfo {
        // The actual data in the field, either a BSON object or a BSONColumn.
        BSONElement data = BSONElement{};
        BSONElement min = BSONElement{};
        BSONElement max = BSONElement{};
    };

    // Populate the 'FieldInfo' data values.
    StringDataMap<FieldInfo> topLevelFieldNameToInfo;
    topLevelFieldNameToInfo.reserve(_topLevelFieldToIdxes.size());
    for (auto elt : bucketDataElem.embeddedObject()) {
        auto it = _topLevelFieldToIdxes.find(elt.fieldNameStringData());
        if (it != _topLevelFieldToIdxes.end()) {
            auto [blockTag, blockVal] = bson::convertFrom<true>(elt);
            tassert(7796400,
                    "Unsupported type for timeseries bucket data",
                    blockTag == value::TypeTags::bsonObject ||
                        blockTag == value::TypeTags::bsonBinData);
            topLevelFieldNameToInfo.emplace(elt.fieldNameStringData(), FieldInfo{.data = elt});
        }
    }

    // Populate min and max for each 'FieldInfo'.
    {
        auto setMinMax = [&topLevelFieldNameToInfo](BSONElement minMaxElt, auto setFn) {
            if (minMaxElt.type() != BSONType::object) {
                return;
            }

            BSONObj minMaxObj = minMaxElt.Obj();
            size_t fieldsFound = 0;
            const size_t nFields = topLevelFieldNameToInfo.size();
            for (BSONElement elt : minMaxObj) {
                if (auto it = topLevelFieldNameToInfo.find(elt.fieldNameStringData());
                    it != topLevelFieldNameToInfo.end()) {
                    setFn(it->second, elt);
                    ++fieldsFound;
                    if (fieldsFound >= nFields) {
                        break;
                    }
                }
            }
        };

        setMinMax(bucketControlObj[timeseries::kBucketControlMinFieldName],
                  [](FieldInfo& fi, BSONElement elt) { fi.min = elt; });
        setMinMax(bucketControlObj[timeseries::kBucketControlMaxFieldName],
                  [](FieldInfo& fi, BSONElement elt) { fi.max = elt; });
    }

    // The time series decoding API gives a couple ways to efficiently decode data:
    // - A whole BSONColumn binary at a time
    // - Extracting paths that identify scalar fields in a BSONColumn, as long as there are not
    //   multi-element arrays along the path
    // For other kinds of paths, we materialize each top level field as BSON, and then extract from
    // that. This is awful in terms of performance, but it can be swapped out with a more efficient
    // implementation when the decoding API can support it efficiently.

    std::vector<std::unique_ptr<TsBlock>> outBlocks;
    std::vector<std::unique_ptr<CellBlock>> outCells(_pathReqs.size());

    for (auto& [topLevelField, fieldInfo] : topLevelFieldNameToInfo) {

        // The set of indexes in _pathReqs which begin with this top level field.
        const auto& pathIndexesForCurrentField = _topLevelFieldToIdxes[topLevelField];
        auto [columnTag, columnVal] = bson::convertFrom<true /*View*/>(fieldInfo.data);

        BSONElement fieldMin = fieldInfo.min;
        BSONElement fieldMax = fieldInfo.max;
        std::pair<TypeTags, Value> controlMin = bson::convertFrom<true /*View*/>(fieldMin);
        std::pair<TypeTags, Value> controlMax = bson::convertFrom<true /*View*/>(fieldMax);

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
            } else if (_pathReqs[idx].path.size() == 3 &&
                       holds_alternative<CellBlock::Get>(_pathReqs[idx].path[0]) &&
                       holds_alternative<CellBlock::Traverse>(_pathReqs[idx].path[1]) &&
                       holds_alternative<CellBlock::Id>(_pathReqs[idx].path[2]) &&
                       (tsBlock->hasNoObjsOrArrays())) {
                // We are traversing a top level field AND there are no arrays. The path must look
                // like: [Get <field> Traverse Id]. If this is the case, we can take a fast path and
                // skip the work of shredding the whole thing.
                //
                // In this case the top level TsCellBlockForTopLevelField (representing the [Get
                // <field> Id]) is identical to the path [Get <field> Traverse Id]. We make a top
                // level cell block with an unowned pointer.
                outCells[idx] = std::make_unique<TsCellBlockForTopLevelField>(tsBlock);
            } else {
                nonTopLevelIdxesForCurrentField.push_back(idx);
            }
        }

        // There are no more paths that were requested which begin with this top level field.
        if (nonTopLevelIdxesForCurrentField.empty()) {
            continue;
        }

        // Try to use path-based decompression if all of the remaining paths refer to scalars in
        // objects in a compressed BSONColumn.
        if (tryPathBasedDecompression(
                *tsBlock, fieldMin, fieldMax, nonTopLevelIdxesForCurrentField, outCells)) {
            // We handled all the remainging paths with fast path-based decompression, so there is
            // no more work to do.
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

bool TsBucketPathExtractor::tryPathBasedDecompression(
    TsBlock& tsBlock,
    const BSONElement fieldMin,
    const BSONElement fieldMax,
    const std::vector<size_t>& nonTopLevelIdxesForCurrentField,
    std::vector<std::unique_ptr<CellBlock>>& outCells) const {

    if (tsBlock.getBlockTag() != TypeTags::bsonBinData) {
        return false;
    }

    if (fieldMin.type() != BSONType::object || fieldMax.type() != BSONType::object) {
        return false;
    }

    ElementAnalysis analysis;
    analyzeElement(analysis, fieldMin);
    analyzeElement(analysis, fieldMax);

    std::vector<bsoncolumn::SBEPath> bsoncolPaths;
    for (size_t idx : nonTopLevelIdxesForCurrentField) {
        auto maybePath = canUsePathBasedDecompression(_pathReqs[idx], analysis, fieldMin, fieldMax);
        if (maybePath) {
            bsoncolPaths.emplace_back(std::move(*maybePath));
        } else {
            break;
        }
    }

    if (bsoncolPaths.size() != nonTopLevelIdxesForCurrentField.size()) {
        return false;
    }

    using SBEMaterializer = sbe::bsoncolumn::SBEColumnMaterializer;

    boost::intrusive_ptr<BSONElementStorage> allocator = new BSONElementStorage{};

    std::vector<BlockBasedDecompressAdaptor> containers;
    std::vector<std::pair<bsoncolumn::SBEPath, BlockBasedDecompressAdaptor&>> pathPairs;
    containers.reserve(bsoncolPaths.size());
    pathPairs.reserve(bsoncolPaths.size());

    size_t noOfMeasurements = tsBlock.count();
    for (auto&& bsoncolPath : bsoncolPaths) {
        containers.emplace_back(noOfMeasurements, allocator);
        pathPairs.emplace_back(std::move(bsoncolPath), containers.back());
    }

    mongo::bsoncolumn::BSONColumnBlockBased col{tsBlock.getBinData()};
    col.decompress<SBEMaterializer>(
        allocator,
        std::span<std::pair<bsoncolumn::SBEPath, BlockBasedDecompressAdaptor&>>{pathPairs});

    for (size_t idx = 0; idx < nonTopLevelIdxesForCurrentField.size(); ++idx) {
        auto cellBlock = std::make_unique<MaterializedCellBlock>();
        cellBlock->_deblocked = containers[idx].extractBlock();
        cellBlock->_filterPosInfo = containers[idx].extractPositionInfo();
        outCells[nonTopLevelIdxesForCurrentField[idx]] = std::move(cellBlock);
    }

    // All paths were handled with path-based decompression.
    return true;
}

TsBlock::TsBlock(size_t ncells,
                 bool owned,
                 TypeTags blockTag,
                 Value blockVal,
                 int bucketVersion,
                 bool isTimeField,
                 std::pair<TypeTags, Value> controlMin,
                 std::pair<TypeTags, Value> controlMax)
    : _blockOwned(owned),
      _blockTag(blockTag),
      _blockVal(blockVal),
      _count(ncells),
      _bucketVersion(bucketVersion),
      _isTimeField(isTimeField),
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
                // the SBE value ownership model. The underlying buffer for the BSON object block
                // is only _sometimes_ owned by this TsBlock so we do not necessarily need to
                // always copy the values out of it. Since deblocking from a BSONObj is the
                // uncommon case compared to deblocking from a BSONColumn, we don't bother with
                // that optimization.
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

    // If we can guarantee there are no arrays nor objects in this column,
    // use the faster block-based decoding API.
    if (hasNoObjsOrArrays()) {
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
        // BSONElementStorageValueBlock which will keep the BSONColumn's BSONElementStorage around.
        // This prevents us from using HomogeneousBlock, but lets us avoid the copy.

        for (size_t i = 0; i < _count; ++i) {
            auto [tag, val] = bson::convertFrom</*View*/ true>(*it);

            // No copy.

            ++it;
            tags.push_back(tag);
            vals.push_back(val);
        }

        // Preserve the storage for this BSONColumn so it lives as long as this TsBlock is alive.
        _decompressedBlock = std::make_unique<BSONElementStorageValueBlock>(
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
                                         _isTimeField);
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

std::pair<TypeTags, Value> TsBlock::tryMax() const {
    auto isControlFieldExact = [&]() {
        // For dates before 1970, the control.max time field is rounded rather than being a value
        // in the bucket. We can't use it as a reliable max if it's before or equal to the Unix
        // epoch.
        // TODO SERVER-94614 use BSONColumn helpers from SERVER-90956 to lazily calculate min/max.
        if (_isTimeField) {
            tassert(9387400,
                    "Expected time field in a time-series collection to always contain a date",
                    _controlMax.first == TypeTags::Date);
            return value::bitcastTo<int64_t>(_controlMax.second) > 0;
        }
        return true;
    };
    if (canUseControlValue(_controlMax.first) && isControlFieldExact()) {
        return _controlMax;
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
    auto precomputedCount = _unownedTsBlock->count();
    auto tsBlockClone = _unownedTsBlock->cloneStrongTyped();

    // Using raw new to access private constructor.
    return std::unique_ptr<TsCellBlockForTopLevelField>(
        new TsCellBlockForTopLevelField(precomputedCount, std::move(tsBlockClone)));
}

TsCellBlockForTopLevelField::TsCellBlockForTopLevelField(TsBlock* block) : _unownedTsBlock(block) {
    // Position info of 1111...
    _positionInfo.resize(block->count(), 1);
}

TsCellBlockForTopLevelField::TsCellBlockForTopLevelField(size_t count,
                                                         std::unique_ptr<TsBlock> tsBlock)
    : _ownedTsBlock(std::move(tsBlock)), _unownedTsBlock(_ownedTsBlock.get()) {
    // Position info of 1111...
    _positionInfo.resize(count, 1);
}
}  // namespace mongo::sbe::value
