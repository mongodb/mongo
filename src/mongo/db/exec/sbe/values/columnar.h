/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/config.h"
#include "mongo/db/exec/sbe/values/column_store_encoder.h"
#include "mongo/db/storage/column_store.h"

/**
 * Helper functions for reading values out of a columnar index for processing in SBE.
 */

namespace mongo::sbe {
/**
 * Represents a cell in a columnar index with ability to retrieve values in an SBE native format.
 */
struct TranslatedCell {
    StringData arrInfo;
    PathView path;

    SplitCellView::Cursor<value::ColumnStoreEncoder> cursor;

    std::pair<value::TypeTags, value::Value> nextValue() {
        auto next = cursor.nextValue();
        invariant(next);
        return *next;
    }

    bool moreValues() const {
        return cursor.hasNext();
    }
};

// For testing only.
struct MockTranslatedCell {
    StringData arrInfo;
    PathView path;

    std::vector<value::TypeTags> types;
    std::vector<value::Value> vals;
    size_t idx = 0;

    std::pair<value::TypeTags, value::Value> nextValue() {
        invariant(idx < vals.size());
        auto ret = std::make_pair(types[idx], vals[idx]);
        vals[idx] = 0;
        types[idx] = value::TypeTags::Nothing;
        ++idx;
        return ret;
    }

    bool moreValues() const {
        return idx < vals.size();
    }
};

namespace value {
/**
 * This struct is used to pass columnstore index cell data to the SBE VM in csiCell value type. The
 * values of this type are fully owned by the column_scan stage and are never created, cloned or
 * destroyed by SBE.
 */
struct CsiCell {
    SplitCellView* splitCellView = nullptr;
    sbe::value::ColumnStoreEncoder* encoder = nullptr;
    FieldIndex pathDepth = 0;
};
}  // namespace value

/**
 * Adds translated cell to an object. This must not be called on an object
 * which has a structure that is incompatible with the structure described in the cell.
 */
template <class T>
void addCellToObject(T& cell, value::Object& out);
}  // namespace mongo::sbe
