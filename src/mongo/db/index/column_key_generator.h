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

#include <iosfwd>

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/storage/column_store.h"
#include "mongo/util/functional.h"

namespace mongo::column_keygen {
/**
 * This is a representation of the cell prior to flattening it out into a buffer which is passed to
 * visitor callbacks.
 *
 * All memory within the UnencodedCellView should only be assumed valid within the callback. If you
 * need it longer, you must copy it yourself. Non-test callers will generally immediately encode
 * this to a flat buffer, so this shouldn't be a problem.
 */
struct UnencodedCellView {
    const std::vector<BSONElement>& vals;
    StringData arrayInfo;

    // If true, this path has multiple values in a single (possibly nested) object with the same
    // field name. In this case, arrayInfo will be empty and this cell must not be used to
    // reconstruct an object. We should probably not attempt to encode vals in the index either, and
    // just put a marker that causes us to either skip the row (because it breaks the rules) or go
    // to the row store.
    //
    // Note that this detection is best-effort and will only detect cases that would result in
    // corrupt array info. We have decided that query results do not need to be precise for objects
    // with duplicate fields, so it is OK if we don't detect every case, as long as we don't crash
    // or cause corruption on the undetected cases.
    bool hasDuplicateFields;

    // If true, this cell omits values that are stored in subpaths.
    bool hasSubPaths;

    // If true, when reconstructing an object, you will need to visit the parent path in order to
    // match current semantics for projections and field-path expressions.
    bool isSparse;

    // If true, at least one of the values in vals is inside of a directly-double-nested array, or
    // the field name was encountered while already inside of a directly-double-nested array, so
    // arrayInfo must be consulted to know which values to skip when matching. If false, simple
    // matches can ignore the array info and just match against each value independently.
    bool hasDoubleNestedArrays;

    // These are only used in tests and for debugging.
    friend bool operator==(const UnencodedCellView&, const UnencodedCellView&);
    friend std::ostream& operator<<(std::ostream&, const UnencodedCellView&);
    friend std::ostream& operator<<(std::ostream&, const UnencodedCellView*);
};

/**
 * Visits all paths within obj and provides their cell values.
 * Path visit order is unspecified.
 */
void visitCellsForInsert(const BSONObj& obj,
                         function_ref<void(PathView, const UnencodedCellView&)> cb);

/**
 * Visits all paths within obj and provides their cell values.
 * Visit order is completely unspecified, so callers should not assume anything, but this function
 * will attempt to perform the visits in an order optimized for inserting into a tree.
 *
 * Current implementation will visit all cells for a given path before moving on to the next path.
 * Additionally, within each path, the cells will be visited in an order matching the order of their
 * corresponding entries in the input vector. This will typically be ordered by RecordId since
 * callers will typically pass records in that order, but this function neither relies on nor
 * ensures that.
 */
void visitCellsForInsert(
    const std::vector<BsonRecord>& recs,
    function_ref<void(PathView, const BsonRecord& record, const UnencodedCellView&)> cb);

/**
 * Visits all paths within obj. When deleting, you do not need to know about values.
 * Path visit order is unspecified.
 */
void visitPathsForDelete(const BSONObj& obj, function_ref<void(PathView)> cb);

/**
 * See visitDiffForUpdate().
 */
enum DiffAction { kInsert, kUpdate, kDelete };

/**
 * Computes differences between oldObj and newObj, and invokes cb() with the required actions to
 * take to update the columnar index.
 *
 * For kInsert and kUpdate, the UnencodedCellView will point to the cell data for newObj (you
 * don't need to know the value for oldObj).
 *
 * For kDelete, the UnencodedCellView pointer will be null.
 *
 * Path visit order is unspecified.
 */
void visitDiffForUpdate(const BSONObj& oldObj,
                        const BSONObj& newObj,
                        function_ref<void(DiffAction, PathView, const UnencodedCellView*)> cb);

}  // namespace mongo::column_keygen
