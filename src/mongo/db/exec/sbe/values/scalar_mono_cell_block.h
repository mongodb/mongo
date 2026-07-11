// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/util/modules.h"

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

    int getApproximateSize() const override {
        return sizeof(*this) + _block.getApproximateSize();
    }

private:
    MonoBlock _block;
    std::vector<int32_t> emptyPositionInfo;
};
}  // namespace mongo::sbe::value
