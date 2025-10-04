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
#include "mongo/db/exec/sbe/values/cell_interface.h"

#include <cstddef>
#include <memory>

namespace mongo::sbe::value {
/**
 * A cell block inside which no cells has an array and all cells are scalar.
 */
class ScalarMonoCellBlock : public CellBlock {
public:
    ScalarMonoCellBlock(size_t count, TypeTags tag, Value val) : _block(count, tag, val) {}
    ScalarMonoCellBlock(MonoBlock b) : _block(std::move(b)) {}

    ValueBlock& getValueBlock() override {
        return _block;
    }

    std::unique_ptr<CellBlock> clone() const override {
        return std::make_unique<ScalarMonoCellBlock>(_block);
    }

    const std::vector<int32_t>& filterPositionInfo() override {
        return emptyPositionInfo;
    }

private:
    MonoBlock _block;
    std::vector<int32_t> emptyPositionInfo;
};
}  // namespace mongo::sbe::value
