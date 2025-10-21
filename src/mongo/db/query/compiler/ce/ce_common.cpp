/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/ce/ce_common.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/path.h"

namespace mongo::ce {

namespace {
class SameSizeVectorBSONElementCmp {
public:
    bool operator()(const std::vector<BSONElement>& l, const std::vector<BSONElement>& r) const {
        // From below, we know these vector are all the same size.
        tassert(11214702, "Vectors must be the same size", l.size() == r.size());

        for (size_t i = 0; i < l.size(); i++) {
            auto cmp = l[i].woCompare(r[i], false, nullptr /* stringComparator */);
            if (cmp != 0) {
                return cmp < 0;
            }
        }
        return false;
    }
};
}  // namespace

// TODO SERVER-112198: Compute all NDVs in a single pass over the sample.
size_t countNDV(const std::vector<FieldPath>& fieldNames, const std::vector<BSONObj>& docs) {
    tassert(11214700, "Field names cannot be empty", !fieldNames.empty());
    std::set<std::vector<BSONElement>, SameSizeVectorBSONElementCmp> distinctValues;

    std::vector<BSONElement> fieldsInDoc;
    fieldsInDoc.reserve(fieldNames.size());

    for (auto&& doc : docs) {
        fieldsInDoc.clear();
        for (const auto& fieldName : fieldNames) {
            // These "array behavior" settings ensure we stop and return any arrays we encounter.
            const ElementPath eltPath(fieldName.fullPath(),
                                      ElementPath::LeafArrayBehavior::kNoTraversal,
                                      ElementPath::NonLeafArrayBehavior::kMatchSubpath);
            BSONElementIterator it(&eltPath, doc);
            tassert(
                11158501, "Should always find at least one element at path in document", it.more());

            const auto elt = it.next();
            tassert(11158502,
                    "Encountered unexpected array in NDV computation",
                    elt.element().type() != BSONType::array);
            fieldsInDoc.push_back(elt.element());
        }

        tassert(11214701,
                "Unexpected number of fields in tuple",
                fieldsInDoc.size() == fieldNames.size());
        distinctValues.insert(fieldsInDoc);
    }
    return distinctValues.size();
}

}  // namespace mongo::ce
