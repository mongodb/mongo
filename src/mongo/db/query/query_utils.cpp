// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/query_utils.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/sbe/match_path.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knobs/query_knob_configuration.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <algorithm>
#include <vector>


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

bool isMatchIdHackEligible(const MatchExpression* me) {
    if (me) {
        const auto* cmpMeBase = dynamic_cast<const ComparisonMatchExpressionBase*>(me);
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
