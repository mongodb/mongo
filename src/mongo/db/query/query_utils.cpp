/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/query/query_utils.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/sbe/match_path.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knob_configuration.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

bool sortPatternHasPartsWithCommonPrefix(const SortPattern& sortPattern) {
    StringDataSet prefixSet;
    for (const auto& part : sortPattern) {
        // Ignore any $meta sorts that may be present.
        if (!part.fieldPath) {
            continue;
        }
        auto [_, inserted] = prefixSet.insert(part.fieldPath->getFieldName(0));
        if (!inserted) {
            return true;
        }
    }
    return false;
}

bool isMatchIdHackEligible(MatchExpression* me) {
    if (me) {
        const auto& cmpMeBase = dynamic_cast<ComparisonMatchExpressionBase*>(me);
        if (!cmpMeBase) {
            return false;
        }

        return me->matchType() == MatchExpression::MatchType::EQ && me->path() == "_id" &&
            Indexability::isExactBoundsGenerating(cmpMeBase->getData());
    }
    return false;
}

bool isSimpleIdQuery(const BSONObj& query) {
    bool hasID = false;

    BSONObjIterator it(query);
    while (it.more()) {
        BSONElement elt = it.next();
        if (elt.fieldNameStringData() == "_id") {
            if (hasID) {
                // we already encountered an _id field
                return false;
            }

            // Verify that the query on _id is a simple equality.
            hasID = true;

            if (elt.type() == BSONType::object) {
                // If the value is an object, it can only have one field and that field can only be
                // a query operator if the operator is $eq.
                if (elt.Obj().firstElementFieldNameStringData().starts_with('$')) {
                    if (elt.Obj().nFields() > 1 ||
                        std::strcmp(elt.Obj().firstElementFieldName(), "$eq") != 0) {
                        return false;
                    }
                    if (!Indexability::isExactBoundsGenerating(elt["$eq"])) {
                        return false;
                    }
                }
            } else if (!Indexability::isExactBoundsGenerating(elt)) {
                // In addition to objects, some other BSON elements are not suitable for exact index
                // lookup.
                return false;
            }
        } else {
            return false;
        }
    }

    return hasID;
}

bool isSortSbeCompatible(const SortPattern& sortPattern) {
    // If the sort has meta or numeric path components, we cannot use SBE.
    return std::all_of(sortPattern.begin(), sortPattern.end(), [](auto&& part) {
        return part.fieldPath &&
            !sbe::MatchPath(part.fieldPath->fullPath()).hasNumericPathComponents();
    });
}
}  // namespace mongo
