// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/cell_interface.h"

#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/value.h"

#include <memory>

namespace mongo::sbe::value {
ValueBlock& MaterializedCellBlock::getValueBlock() {
    tassert(7953701, "Value block should be non null", _deblocked);
    return *_deblocked;
}
std::unique_ptr<CellBlock> MaterializedCellBlock::clone() const {
    auto ret = std::make_unique<MaterializedCellBlock>();
    ret->_deblocked = _deblocked->clone();
    ret->_filterPosInfo = _filterPosInfo;
    return ret;
}
}  // namespace mongo::sbe::value
