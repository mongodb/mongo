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

#include "mongo/db/index/wildcard_validation.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>

// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index_names.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {
static const StringData idFieldName = "_id";
/*
 * Validate that wildcatdProject fields have no overlapping. It takes a sorted list of the
 * projection fields.
 */
Status validateOverlappingFieldsInWildcardProjectionOnly(
    const std::vector<FieldRef>& projectionFields) {
    for (size_t i = 1; i < projectionFields.size(); ++i) {
        if (projectionFields[i - 1].isPrefixOfOrEqualTo(projectionFields[i]) ||
            projectionFields[i].isPrefixOfOrEqualTo(projectionFields[i - 1])) {
            return {ErrorCodes::Error{7246200},
                    str::stream() << "Fields in Wildcard Projection cannot overlap, however '"
                                  << projectionFields[i - 1].dottedField()
                                  << "' is overlapped with '" << projectionFields[i].dottedField()};
        }
    }

    return Status::OK();
}

/*
 * Parse wildcard index's keyPattern.
 * It performs basic validation and returns error if the validation fails.
 */
Status getWildcardIndexKeyFields(const BSONObj& keyPattern,
                                 FieldRef& wildcardField,
                                 std::vector<FieldRef>& indexFields) {
    for (const auto& keyElement : keyPattern) {
        auto keyElemFieldName = keyElement.fieldNameStringData();
        if (WildcardNames::isWildcardFieldName(keyElemFieldName)) {
            if (!wildcardField.empty()) {
                return {ErrorCodes::Error{7246201},
                        "Index Key can contain only one wildcard index field"};
            }
            wildcardField = FieldRef{std::move(keyElemFieldName)};
        } else {
            indexFields.emplace_back(std::move(keyElemFieldName));
        }
    }

    if (wildcardField.empty()) {
        return {ErrorCodes::Error{7246202},
                "No wildcard index field is specified for the wildcard index"};
    }

    return Status::OK();
}
}  // namespace

Status validateWildcardIndex(const BSONObj& keyPattern) {
    FieldRef wildcardRef{};
    std::vector<FieldRef> indexFields;
    auto status = getWildcardIndexKeyFields(keyPattern, wildcardRef, indexFields);
    if (!status.isOK()) {
        return status;
    }

    wildcardRef.removeLastPart();  // remove wildcard part

    // wildcard should not overlap with index key fields if wildcard projection is empty.
    for (const auto& field : indexFields) {
        if (wildcardRef.isPrefixOfOrEqualTo(field)) {
            return Status(
                ErrorCodes::Error{7246204},
                str::stream()
                    << "A wildcard index field should not overlap with and index key fields if "
                       "wildcard projection is empty, however '"
                    << field.dottedField() << "' is overlapping with the wildcard index field.");
        }
    }

    return Status::OK();
}

Status validateWildcardProjection(const BSONObj& keyPattern, const BSONObj& pathProjection) {
    if (pathProjection.isEmpty()) {
        return {ErrorCodes::Error{7246205}, "WildcardProjection must be non-empty if specified."};
    }

    // Prepare data for validation.
    FieldRef wildcardField{};
    std::vector<FieldRef> indexFields;
    auto status = getWildcardIndexKeyFields(keyPattern, wildcardField, indexFields);
    if (!status.isOK()) {
        return status;
    }

    if (wildcardField.numParts() != 1) {
        return {
            ErrorCodes::Error{7246206},
            str::stream()
                << "Wildcard index field must be always '$**' if wildcardProjection is specified"};
    }

    std::vector<FieldRef> projectionIncludedFields;
    std::vector<FieldRef> projectionExcludedFields;
    for (const auto& projectionElement : pathProjection) {
        if (projectionElement.trueValue()) {
            projectionIncludedFields.emplace_back(projectionElement.fieldNameStringData());
        } else {
            projectionExcludedFields.emplace_back(projectionElement.fieldNameStringData());
        }
    }

    std::sort(indexFields.begin(), indexFields.end());
    std::sort(projectionIncludedFields.begin(), projectionIncludedFields.end());
    std::sort(projectionExcludedFields.begin(), projectionExcludedFields.end());

    status = validateOverlappingFieldsInWildcardProjectionOnly(projectionIncludedFields);
    if (!status.isOK()) {
        return status;
    }

    status = validateOverlappingFieldsInWildcardProjectionOnly(projectionExcludedFields);
    if (!status.isOK()) {
        return status;
    }

    // test overlappings between index keys and wildcard projection
    {
        auto indexPos = indexFields.begin();
        auto projectionPos = projectionIncludedFields.begin();
        while (indexPos != indexFields.end() && projectionPos != projectionIncludedFields.end()) {
            if (projectionPos->isPrefixOfOrEqualTo(*indexPos)) {
                return {ErrorCodes::Error{7246208},
                        str::stream()
                            << "Index Key and Wildcard Projection cannot contain "
                               "overlapping fields, however '"
                            << indexPos->dottedField() << "' index field is ovverlapping with '"
                            << projectionPos->dottedField() << "' wildcardProjection path."};
            }

            int cmp = projectionPos->compare(*indexPos);
            if (cmp < 0) {
                ++projectionPos;
            } else {
                ++indexPos;
            }
        }
    }

    const FieldRef idFieldRef{idFieldName};
    const bool idOnlyExclusion =
        projectionExcludedFields.size() == 1 && projectionExcludedFields.front() == idFieldRef;

    // test test wildcard projects exclude all regular index fields
    if (!projectionExcludedFields.empty() && !idOnlyExclusion) {
        auto indexPos = indexFields.begin();
        auto projectionPos = projectionExcludedFields.begin();
        while (indexPos != indexFields.end() && projectionPos != projectionExcludedFields.end()) {
            if (projectionPos->isPrefixOfOrEqualTo(*indexPos)) {
                // fine we can move indexPos to test next index field
                ++indexPos;
            } else {
                int cmp = projectionPos->compare(*indexPos);
                if (cmp < 0) {
                    ++projectionPos;
                } else {
                    return {ErrorCodes::Error{7246209},
                            str::stream() << "wildcardProjection paths must exclude all regular "
                                             "index fields, however '"
                                          << indexPos->dottedField() << "'is not excluded."};
                }
            }
        }

        if (indexPos != indexFields.end()) {
            return {ErrorCodes::Error{7246210},
                    str::stream() << "wildcardProjection paths must exclude all regular "
                                     "index fields, however '"
                                  << indexPos->dottedField() << "'is not excluded."};
        }
    }

    // With the exception of explicitly including _id field, you cannot combine inclusion and
    // exclusion statements in the wildcardProjection document.
    if (!projectionIncludedFields.empty() && !projectionExcludedFields.empty()) {
        const bool idOnlyInclusion =
            projectionIncludedFields.size() == 1 && projectionIncludedFields.front() == idFieldRef;
        const bool idIsSingleField = idOnlyExclusion || idOnlyInclusion;
        if (!idIsSingleField) {
            return {
                ErrorCodes::Error{7246211},
                str::stream()
                    << "Inclusion and exclusion statements cannot combine in the "
                       "wildcardProjection with an exception of explicitly including _id field"};
        }
    }

    return Status::OK();
}
}  // namespace mongo
