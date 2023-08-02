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
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/util.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/util/itoa.h"

namespace mongo::sbe::value {
TsBlock::TsBlock(bool owned, TypeTags blockTag, Value blockVal)
    : _blockOwned(owned), _blockTag(blockTag), _blockVal(blockVal) {
    invariant(_blockTag == TypeTags::bsonObject || _blockTag == TypeTags::bsonBinData);
}

TsBlock::~TsBlock() {
    if (_blockOwned) {
        // The underlying buffer is owned by this TsBlock and so this releases it.
        releaseValue(_blockTag, _blockVal);
    }

    // Deblocked values are owned by this TsBlock and so this releases them.
    for (size_t i = 0; i < _deblockedTags.size(); ++i) {
        releaseValue(_deblockedTags[i], _deblockedVals[i]);
    }
}

void TsBlock::deblockFromBsonObj() {
    ObjectEnumerator enumerator(TypeTags::bsonObject, _blockVal);
    for (size_t i = 0; !enumerator.atEnd(); ++i) {
        auto [tag, val] = [&]() -> std::pair<TypeTags, Value> {
            if (ItoA(i) != enumerator.getFieldName()) {
                // There's a missing index or a hole in the middle, so returns Nothing.
                return {TypeTags::Nothing, Value(0)};
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
        _deblockedTags.push_back(tag);
        _deblockedVals.push_back(val);
        guard.reset();
    }
}

void TsBlock::deblockFromBsonColumn() {
    tassert(7796401,
            "Invalid BinDataType for BSONColumn",
            getBSONBinDataSubtype(TypeTags::bsonBinData, _blockVal) == BinDataType::Column);
    BSONColumn blockColumn(
        BSONBinData{value::getBSONBinData(TypeTags::bsonBinData, _blockVal),
                    static_cast<int>(value::getBSONBinDataSize(TypeTags::bsonBinData, _blockVal)),
                    BinDataType::Column});
    for (auto& elem : blockColumn) {
        // BSONColumn::Iterator decompresses values into its own buffer which is invalidated
        // whenever the iterator advances, so we need to copy them out.
        auto [tag, val] = bson::convertFrom</*View*/ false>(elem);
        ValueGuard guard(tag, val);
        _deblockedTags.push_back(tag);
        _deblockedVals.push_back(val);
        guard.reset();
    }
}

std::unique_ptr<ValueBlock> TsBlock::clone() const {
    auto [cpyTag, cpyVal] = copyValue(_blockTag, _blockVal);
    ValueGuard guard(cpyTag, cpyVal);
    // The new copy must own the copied underlying buffer.
    auto cpy = std::make_unique<TsBlock>(/*owned*/ true, cpyTag, cpyVal);
    guard.reset();

    if (!_deblockedTags.empty()) {
        // If the block has been deblocked, then we need to copy the deblocked values too to
        // avoid deblocking overhead again. The new copy must own the copied deblocked values.
        cpy->_deblockedTags.reserve(_deblockedTags.size());
        cpy->_deblockedVals.reserve(_deblockedVals.size());
        for (size_t i = 0; i < _deblockedTags.size(); ++i) {
            auto [cpyTag, cpyVal] = copyValue(_deblockedTags[i], _deblockedVals[i]);
            ValueGuard deblockedValueGuard(cpyTag, cpyVal);
            cpy->_deblockedTags.push_back(cpyTag);
            cpy->_deblockedVals.push_back(cpyVal);
            deblockedValueGuard.reset();
        }
    }

    return cpy;
}

const ValueBlock& TsCellBlock::getValueBlock() const {
    return static_cast<const ValueBlock&>(_tsBlock);
}

// The 'TsCellBlock' never owns the 'topLevelVal' and so it is always a view on the BSON provided
// by the stage tree below.
TsCellBlock::TsCellBlock(bool owned, TypeTags topLevelTag, Value topLevelVal)
    : _blockTag(topLevelTag), _blockVal(topLevelVal), _tsBlock(owned, topLevelTag, topLevelVal) {
    invariant(_blockTag == value::TypeTags::bsonObject ||
              _blockTag == value::TypeTags::bsonBinData);
}
}  // namespace mongo::sbe::value
