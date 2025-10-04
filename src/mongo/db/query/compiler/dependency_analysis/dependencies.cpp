/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <bitset>
#include <compare>

namespace mongo {

OrderedPathSet DepsTracker::simplifyDependencies(const OrderedPathSet& dependencies,
                                                 TruncateToRootLevel truncateToRootLevel) {
    // The key operation here is folding dependencies into ancestor dependencies, wherever possible.
    // This is assisted by a special sort in OrderedPathSet that treats '.'
    // as the first char and thus places parent paths directly before their children.
    OrderedPathSet returnSet;
    std::string last;
    for (const auto& path : dependencies) {
        if (!last.empty() && str::startsWith(path, last)) {
            // We are including a parent of this field, so we can skip this field.
            continue;
        }

        // Check that the field requested is a valid field name in the agg language. This
        // constructor will throw if it isn't.
        FieldPath fp(path);

        if (truncateToRootLevel == TruncateToRootLevel::yes) {
            last = std::string{fp.front()} + '.';
            returnSet.insert(std::string{fp.front()});
        } else {
            last = path + '.';
            returnSet.insert(path);
        }
    }
    return returnSet;
}

BSONObj DepsTracker::toProjectionWithoutMetadata(
    TruncateToRootLevel truncationBehavior /*= TruncateToRootLevel::no*/) const {
    BSONObjBuilder bb;

    if (needWholeDocument) {
        return bb.obj();
    }

    if (fields.empty()) {
        // We need no user-level fields (as we would if this was logically a count). Since there is
        // no way of expressing a projection that indicates no depencies, we return an empty
        // projection.
        return bb.obj();
    }

    // Create a projection from the simplified dependencies (absorbing descendants into parents).
    // For example, the dependencies ["a.b", "a.b.c.g", "c", "c.d", "f"] would be
    // minimally covered by the projection {"a.b": 1, "c": 1, "f": 1}.
    bool idSpecified = false;
    for (auto& path : simplifyDependencies(fields, truncationBehavior)) {
        // Remember if _id was specified.  If not, we'll later explicitly add {_id: 0}
        if (str::startsWith(path, "_id") && (path.size() == 3 || path[3] == '.')) {
            idSpecified = true;
        }
        bb.append(path, 1);
    }

    if (!idSpecified) {
        bb.append("_id", 0);
    }

    return bb.obj();
}

void DepsTracker::setNeedsMetadata(DocumentMetadataFields::MetaType type) {
    static const std::set<DocumentMetadataFields::MetaType> kMetadataFieldsToBeValidated = {
        DocumentMetadataFields::MetaType::kTextScore,
        DocumentMetadataFields::MetaType::kGeoNearDist,
        DocumentMetadataFields::MetaType::kGeoNearPoint,
        DocumentMetadataFields::MetaType::kScore,
        DocumentMetadataFields::MetaType::kScoreDetails,
        DocumentMetadataFields::MetaType::kSearchScore,
        DocumentMetadataFields::MetaType::kVectorSearchScore,
        DocumentMetadataFields::MetaType::kSearchRootDocumentId,
    };

    // Perform validation if necessary.
    if (!std::holds_alternative<NoMetadataValidation>(_availableMetadata) &&
        kMetadataFieldsToBeValidated.contains(type)) {
        auto& availableMetadataBitSet = std::get<QueryMetadataBitSet>(_availableMetadata);
        uassert(40218,
                str::stream() << "query requires " << type << " metadata, but it is not available",
                availableMetadataBitSet[type]);
    }
    _metadataDeps[type] = true;
}

void DepsTracker::setNeedsMetadata(const QueryMetadataBitSet& metadata) {
    for (size_t i = 1; i < DocumentMetadataFields::kNumFields; ++i) {
        if (metadata[i]) {
            setNeedsMetadata(static_cast<DocumentMetadataFields::MetaType>(i));
        }
    }
}

void DepsTracker::setMetadataAvailable(DocumentMetadataFields::MetaType type) {
    if (std::holds_alternative<NoMetadataValidation>(_availableMetadata)) {
        return;
    }

    auto& availableMetadataBitSet = std::get<QueryMetadataBitSet>(_availableMetadata);
    availableMetadataBitSet[type] = true;

    // Some meta types are alias'd to others (for example, "textScore" is also available via
    // "score"), so we must mark those alias'd types as available too.
    switch (type) {
        case DocumentMetadataFields::MetaType::kTextScore:
        case DocumentMetadataFields::MetaType::kSearchScore:
        case DocumentMetadataFields::MetaType::kVectorSearchScore:
        // Setting "scoreDetails" will also set "score".
        case DocumentMetadataFields::MetaType::kScoreDetails:
            availableMetadataBitSet[DocumentMetadataFields::MetaType::kScore] = true;
            break;
        case DocumentMetadataFields::MetaType::kSearchScoreDetails:
            availableMetadataBitSet[DocumentMetadataFields::MetaType::kScoreDetails] = true;
            break;
        default:
            break;
    }
}

void DepsTracker::setMetadataAvailable(const QueryMetadataBitSet& metadata) {
    for (size_t i = 1; i < DocumentMetadataFields::kNumFields; ++i) {
        if (metadata[i]) {
            setMetadataAvailable(static_cast<DocumentMetadataFields::MetaType>(i));
        }
    }
}

void DepsTracker::clearMetadataAvailable() {
    // TODO SERVER-100443 Right now we only clear "score" and "scoreDetails", but we should be able
    // to reset the entire bit set.

    std::visit(OverloadedVisitor{
                   [](NoMetadataValidation) {},
                   [](auto& availableMetadataBitSet) {
                       availableMetadataBitSet[DocumentMetadataFields::kScore] = false;
                       availableMetadataBitSet[DocumentMetadataFields::kScoreDetails] = false;
                   },
               },
               _availableMetadata);
}

// Returns true if the lhs value should sort before the rhs, false otherwise.
bool PathComparator::operator()(StringData lhs, StringData rhs) const {
    // Use the three-way (<=>) comparator to avoid code duplication.
    return std::is_lt(ThreeWayPathComparator{}(lhs, rhs));
}

std::strong_ordering ThreeWayPathComparator::operator()(StringData lhs, StringData rhs) const {
    constexpr char dot = '.';

    for (size_t pos = 0, len = std::min(lhs.size(), rhs.size()); pos < len; ++pos) {
        // Below, we explicitly choose unsigned char because the usual const char& returned by
        // operator[] is actually signed on x86 and will incorrectly order unicode characters.
        unsigned char lchar = lhs[pos], rchar = rhs[pos];

        const auto res = lchar <=> rchar;
        if (std::is_eq(res)) {
            continue;
        }

        // Consider the path delimiter '.' as being less than all other characters, so that
        // paths sort directly before any paths they prefix and directly after any paths
        // which prefix them.
        if (lchar == dot) {
            return std::strong_ordering::less;
        } else if (rchar == dot) {
            return std::strong_ordering::greater;
        }

        // Otherwise, default to normal character comparison.
        return res;
    }

    // If we get here, then we have reached the end of lhs and/or rhs and all of their path
    // segments up to this point match. If lhs is shorter than rhs, then lhs prefixes rhs
    // and should sort before it.
    return lhs.size() <=> rhs.size();
}

bool DepsTracker::needsTextScoreMetadata(const QueryMetadataBitSet& metadataDeps) {
    // Users may request the score with either $textScore or $score.
    return metadataDeps[DocumentMetadataFields::kTextScore] ||
        metadataDeps[DocumentMetadataFields::kScore];
}

}  // namespace mongo
