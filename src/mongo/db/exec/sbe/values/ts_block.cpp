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
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/scalar_mono_cell_block.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/timeseries/bucket_unpacker.h"
#include "mongo/util/itoa.h"

namespace mongo::sbe::value {

TsBucketPathExtractor::TsBucketPathExtractor(std::vector<CellBlock::PathRequest> pathReqs,
                                             StringData timeField)
    : _pathReqs(std::move(pathReqs)), _timeField(timeField) {

    size_t idx = 0;
    for (auto& req : _pathReqs) {
        tassert(7796405,
                "Path must start with a Get operation",
                std::holds_alternative<CellBlock::Get>(req.path[0]));

        StringData field = std::get<CellBlock::Get>(req.path[0]).field;
        _topLevelFieldToIdxes[field].push_back(idx);

        if (req.path.size() > 2) {
            _topLevelFieldsWithSubfieldAccess.insert(field.toString());
            _nonTopLevelPathReqs.push_back(req);
            _nonTopLevelPathIdxes.push_back(idx);
        }

        ++idx;
    }
}

std::vector<std::unique_ptr<CellBlock>> TsBucketPathExtractor::extractCellBlocks(
    const BSONObj& bucketObj) {

    BSONElement bucketControl = bucketObj[timeseries::kBucketControlFieldName];
    invariant(!bucketControl.eoo());


    const int noOfMeasurements = [&]() {
        if (auto ct = bucketControl.Obj()[timeseries::kBucketControlCountFieldName]) {
            return static_cast<int>(ct.numberLong());
        }
        return timeseries::BucketUnpacker::computeMeasurementCount(bucketObj,
                                                                   StringData(_timeField));
    }();

    BSONElement data = bucketObj[timeseries::kBucketDataFieldName];
    invariant(!data.eoo());
    invariant(data.type() == BSONType::Object);

    StringMap<BSONElement> topLevelFieldToBsonElt;
    for (auto elt : data.embeddedObject()) {
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

    std::vector<BSONObjBuilder> bsonBuilders(noOfMeasurements);
    std::vector<BSONObj> bsons(noOfMeasurements);

    std::vector<std::unique_ptr<CellBlock>> out(_pathReqs.size());

    for (auto& [topLevelField, elt] : topLevelFieldToBsonElt) {
        const auto& indexes = _topLevelFieldToIdxes[topLevelField];
        auto [blockTag, blockVal] = bson::convertFrom<true>(elt);

        std::vector<size_t> nonTopLevelIdxes;
        for (auto idx : indexes) {
            bool isTopLevelOnly =
                std::find(_nonTopLevelPathIdxes.begin(), _nonTopLevelPathIdxes.end(), idx) ==
                _nonTopLevelPathIdxes.end();

            if (isTopLevelOnly) {
                out[idx] = std::make_unique<value::TsCellBlock>(
                    noOfMeasurements, /*owned*/ false, blockTag, blockVal);
            } else {
                nonTopLevelIdxes.push_back(idx);
            }
        }

        if (nonTopLevelIdxes.empty()) {
            continue;
        }

        // Otherwise we'll shred this one
        if (elt.type() == BSONType::BinData) {
            BSONColumn column(elt);
            size_t columnIdx = 0;
            for (auto columnElt : column) {
                if (columnElt) {
                    bsonBuilders[columnIdx].appendAs(columnElt, elt.fieldNameStringData());
                }

                ++columnIdx;
            }
        } else {
            size_t idx = 0;
            for (auto columnElt : elt.embeddedObject()) {
                while (ItoA(idx) != columnElt.fieldNameStringData()) {
                    ++idx;
                }

                bsonBuilders[idx].appendAs(columnElt, elt.fieldNameStringData());
            }

            // Final gap between idx and noOfMeasurements can be left empty.
        }

        for (size_t i = 0; i < bsons.size(); ++i) {
            bsons[i] = bsonBuilders[i].asTempObj();
        }

        std::vector<CellBlock::PathRequest> reqs;
        for (auto idx : nonTopLevelIdxes) {
            reqs.push_back(_pathReqs[idx]);
        }
        auto cellsForNestedFields = value::extractCellBlocksFromBsons(reqs, bsons);

        for (size_t i = 0; i < cellsForNestedFields.size(); ++i) {
            out[nonTopLevelIdxes[i]] = std::move(cellsForNestedFields[i]);
        }

        for (auto& bob : bsonBuilders) {
            bob.resetToEmpty();
        }
    }

    for (auto& cellBlock : out) {
        if (!cellBlock) {
            auto emptyBlock = std::make_unique<value::ScalarMonoCellBlock>(
                noOfMeasurements, value::TypeTags::Nothing, value::Value(0));
            cellBlock = std::move(emptyBlock);
        }
    }

    return out;
}

TsBlock::TsBlock(size_t ncells, bool owned, TypeTags blockTag, Value blockVal)
    : _blockOwned(owned), _blockTag(blockTag), _blockVal(blockVal), _count(ncells) {
    invariant(_blockTag == TypeTags::bsonObject || _blockTag == TypeTags::bsonBinData);
}

TsBlock::~TsBlock() {
    if (_blockOwned) {
        // The underlying buffer is owned by this TsBlock and so this releases it.
        releaseValue(_blockTag, _blockVal);
    }
}

void TsBlock::deblockFromBsonObj(std::vector<TypeTags>& deblockedTags,
                                 std::vector<Value>& deblockedVals) const {
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

        ValueGuard guard(tag, val);
        deblockedTags.push_back(tag);
        deblockedVals.push_back(val);
        guard.reset();
    }
}

void TsBlock::deblockFromBsonColumn(std::vector<TypeTags>& deblockedTags,
                                    std::vector<Value>& deblockedVals) const {
    tassert(7796401,
            "Invalid BinDataType for BSONColumn",
            getBSONBinDataSubtype(TypeTags::bsonBinData, _blockVal) == BinDataType::Column);
    BSONColumn blockColumn(
        BSONBinData{value::getBSONBinData(TypeTags::bsonBinData, _blockVal),
                    static_cast<int>(value::getBSONBinDataSize(TypeTags::bsonBinData, _blockVal)),
                    BinDataType::Column});
    auto it = blockColumn.begin();
    for (size_t i = 0; i < _count; ++i) {
        // BSONColumn::Iterator decompresses values into its own buffer which is invalidated
        // whenever the iterator advances, so we need to copy them out.
        auto [tag, val] = bson::convertFrom</*View*/ false>(*it);
        ++it;

        ValueGuard guard(tag, val);
        deblockedTags.push_back(tag);
        deblockedVals.push_back(val);
        guard.reset();
    }
}

std::unique_ptr<ValueBlock> TsBlock::clone() const {
    auto [cpyTag, cpyVal] = copyValue(_blockTag, _blockVal);
    ValueGuard guard(cpyTag, cpyVal);
    // The new copy must own the copied underlying buffer.
    auto cpy = std::make_unique<TsBlock>(_count, /*owned*/ true, cpyTag, cpyVal);
    guard.reset();

    // If the block has been deblocked, then we need to copy the deblocked values too to
    // avoid deblocking overhead again.
    cpy->_deblockedStorage = _deblockedStorage;

    return cpy;
}

ValueBlock& TsCellBlock::getValueBlock() {
    return _tsBlock;
}

std::unique_ptr<CellBlock> TsCellBlock::clone() const {
    auto [cpyTag, cpyVal] = value::copyValue(_blockTag, _blockVal);
    auto precomputedCount = _tsBlock.tryCount();
    tassert(
        7943900, "Assumes count() is available in O(1) time on TS Block type", precomputedCount);
    return std::make_unique<TsCellBlock>(*precomputedCount, true /* owned */, cpyTag, cpyVal);
}

// The 'TsCellBlock' never owns the 'topLevelVal' and so it is always a view on the BSON provided
// by the stage tree below.
TsCellBlock::TsCellBlock(size_t count, bool owned, TypeTags topLevelTag, Value topLevelVal)
    : _blockTag(topLevelTag),
      _blockVal(topLevelVal),
      // The 'count' means the number of cells in this TsCellBlock and as of now, we only support
      // top-level fields only, the number of values per cell is always 1 and the number of cells
      // in this TsCellBlock is always the same as the number of values in '_tsBlock'. So, we pass
      // 'count' to '_tsBlock' as the number of values in it.
      _tsBlock(count, owned, topLevelTag, topLevelVal) {
    invariant(_blockTag == value::TypeTags::bsonObject ||
              _blockTag == value::TypeTags::bsonBinData);

    _positionInfo.resize(count, char(1));
}
}  // namespace mongo::sbe::value
