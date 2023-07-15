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
TsBlock::TsBlock(bool owned, value::TypeTags blockTag, value::Value blockVal)
    : _owned(owned), _blockTag(blockTag), _blockVal(blockVal) {
    invariant(_blockTag == value::TypeTags::bsonObject ||
              _blockTag == value::TypeTags::bsonBinData);
}

TsBlock::~TsBlock() {
    if (_owned) {
        releaseValue(_blockTag, _blockVal);
    }
}

boost::optional<std::pair<TypeTags, Value>> TsBlock::Cursor::next() {
    if (_enumerator.atEnd()) {
        return boost::none;
    }

    if (ItoA(_reqIndex++) != _enumerator.getFieldName()) {
        // We're not at the index that is requested, so returns Nothing.
        return std::make_pair(TypeTags::Nothing, Value(0));
    }

    auto [tag, val] = _enumerator.getViewOfValue();
    _enumerator.advance();
    return std::make_pair(tag, val);
}

boost::optional<std::pair<TypeTags, Value>> TsBlock::BsonColumnCursor::next() {
    if (_alreadyCalledBefore) {
        ++_it;
    } else {
        // We should not advance the iterator on the first call to next(). See
        // '_alreadyCalledBefore' for more details
        _alreadyCalledBefore = true;
    }

    if (_it == _bsonColumn.end()) {
        return boost::none;
    }

    // BSONColumn::Iterator returns a BSONElement which will stay valid until the next call to
    // operator++.
    std::tie(_curTag, _curVal) = bson::convertFrom<true>(*_it);
    return std::make_pair(_curTag, _curVal);
}

const ValueBlock& TsCellBlock::getValueBlock() const {
    ensureValueBlockExists();
    return *_valueBlock;
}

void TsCellBlock::ensureValueBlockExists() const {
    if (_valueBlock) {
        // Already exists, don't recreate.
        return;
    }

    // This TsCellBlock or the storage engine owns the underlying storage buffer. Hence, the
    // ValueBlock must not own it.
    _valueBlock.emplace(/*owned*/ false, _blockTag, _blockVal);
}

TsCellBlock::TsCellBlock(bool owned, value::TypeTags topLevelTag, value::Value topLevelVal)
    : _owned(owned), _blockTag(topLevelTag), _blockVal(topLevelVal) {
    invariant(_blockTag == value::TypeTags::bsonObject ||
              _blockTag == value::TypeTags::bsonBinData);
}

TsCellBlock::~TsCellBlock() {
    if (_owned) {
        releaseValue(_blockTag, _blockVal);
    }
}
}  // namespace mongo::sbe::value
