/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <algorithm>
#include <map>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/db/exec/sbe/values/block_interface.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe {
static std::pair<value::TypeTags, value::Value> makeInt32(int32_t value) {
    return {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(value)};
}

static std::vector<std::pair<value::TypeTags, value::Value>> makeInt32s(
    std::vector<int32_t> values) {
    std::vector<std::pair<value::TypeTags, value::Value>> ints;
    for (auto v : values) {
        ints.push_back(makeInt32(v));
    }
    return ints;
}

static std::unique_ptr<value::HeterogeneousBlock> makeHeterogeneousBlock(
    std::vector<std::pair<value::TypeTags, value::Value>> vals) {
    auto block = std::make_unique<value::HeterogeneousBlock>();
    for (auto [t, v] : vals) {
        block->push_back(t, v);
    }
    return block;
}

static std::pair<value::TypeTags, value::Value> makeHeterogeneousBlockTagVal(
    std::vector<std::pair<value::TypeTags, value::Value>> vals) {
    auto block = makeHeterogeneousBlock(vals);
    auto resultVal = value::bitcastFrom<value::ValueBlock*>(block.release());
    return {sbe::value::TypeTags::valueBlock, resultVal};
}

static std::unique_ptr<value::ValueBlock> makeHeterogeneousBoolBlock(std::vector<bool> bools) {
    auto block = std::make_unique<value::HeterogeneousBlock>();
    for (auto b : bools) {
        block->push_back(value::TypeTags::Boolean, value::bitcastFrom<bool>(b));
    }
    return block;
}

static std::unique_ptr<value::ValueBlock> makeBoolBlock(std::vector<bool> bools) {
    return std::make_unique<value::BoolBlock>(bools);
}
}  // namespace mongo::sbe
