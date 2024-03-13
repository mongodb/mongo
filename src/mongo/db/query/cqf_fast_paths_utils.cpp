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

#include "mongo/db/query/cqf_fast_paths_utils.h"

namespace mongo::optimizer::fast_path {
namespace {

bool isSpecialField(StringData field) {
    return field == "_id" || field.find(".") != std::string::npos;
}

bool isExpression(StringData field) {
    return !field.empty() && field[0] == '$';
}

}  // namespace

const FilterComparator FilterComparator::kInstance{};

int FilterComparator::compare(const BSONObj& lhs, const BSONObj& rhs) const {
    for (BSONObjIterator lhsIt{lhs}, rhsIt{rhs};;) {
        const auto& lhsChild = lhsIt.next();
        const auto& rhsChild = rhsIt.next();

        if (lhsChild.eoo() && rhsChild.eoo()) {
            return 0;
        }
        if (lhsChild.eoo()) {
            return -1;
        }
        if (rhsChild.eoo()) {
            return 1;
        }

        const auto& lhsField = lhsChild.fieldNameStringData();
        const auto& rhsField = rhsChild.fieldNameStringData();

        if (isSpecialField(lhsField) || isSpecialField(rhsField)) {
            // We currently don't want to match predicates on the '_id' field or a dotted field
            // (e.g. '{"a.b.c": 1}').
            return lhsChild.woCompare(rhsChild);
        }

        if (isExpression(lhsField) || isExpression(rhsField)) {
            // Ensure we have the same expression on both sides.
            if (auto diff = lhsField.compare(rhsField)) {
                return diff;
            }
            if (containsExpression(lhsChild) || containsExpression(rhsChild)) {
                // We have nested expressions, do a strict comparison. Note that while we don't
                // currently want to match nested expressions, the comparator still needs to
                // implement a strict weak ordering between predicates.
                return lhsChild.woCompare(rhsChild);
            }
        } else if (lhsChild.isABSONObj() && rhsChild.isABSONObj()) {
            if (auto diff = compare(lhsChild.Obj(), rhsChild.Obj())) {
                return diff;
            }
        } else if (lhsChild.isABSONObj() || rhsChild.isABSONObj()) {
            // Different shape before reaching an expression.
            return lhsChild.woCompare(rhsChild);
        }
        // Non-object constants are treated as equal.
    }

    MONGO_UNREACHABLE_TASSERT(8321500);
}

bool FilterComparator::containsExpression(const BSONObj& obj) const {
    for (auto&& elem : obj) {
        if (isExpression(elem.fieldNameStringData())) {
            return true;
        }
        if (containsExpression(elem)) {
            return true;
        }
    }
    return false;
}

bool FilterComparator::containsExpression(const BSONElement& elem) const {
    return elem.valueStringDataSafe().startsWith("$") ||
        (elem.isABSONObj() && containsExpression(elem.Obj()));
}

bool containsSpecialField(const BSONObj& obj) {
    for (auto&& elem : obj) {
        if (isSpecialField(elem.fieldNameStringData())) {
            return true;
        }
        if (elem.isABSONObj() && containsSpecialField(elem.Obj())) {
            return true;
        }
    }
    return false;
}

}  // namespace mongo::optimizer::fast_path
