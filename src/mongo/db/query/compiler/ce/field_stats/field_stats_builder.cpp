// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/field_stats/field_stats_builder.h"

#include "mongo/db/query/compiler/ce/field_stats/field_stats.h"
#include "mongo/db/query/compiler/stats/field_values.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace mongo::ce {
namespace {

std::vector<FieldPath> normalizeFieldStatsKeyPaths(const std::vector<std::string>& fieldPaths) {
    uassert(13176201,
            str::stream() << "fieldStats mode supports between 1 and " << kMaxFieldStatsFields
                          << " key paths",
            !fieldPaths.empty() && fieldPaths.size() <= kMaxFieldStatsFields);

    std::vector<FieldPath> parsedPaths;
    parsedPaths.reserve(fieldPaths.size());
    for (const auto& path : fieldPaths) {
        validateFieldStatsPath(path);
        parsedPaths.emplace_back(path);
    }
    std::sort(parsedPaths.begin(), parsedPaths.end());

    // Compare every pair: "a-b" sorts between "a" and "a.b".
    for (size_t leftIndex = 0; leftIndex < parsedPaths.size(); ++leftIndex) {
        const auto& leftPath = parsedPaths[leftIndex];
        for (size_t rightIndex = leftIndex + 1; rightIndex < parsedPaths.size(); ++rightIndex) {
            const auto& rightPath = parsedPaths[rightIndex];
            uassert(13176202, "fieldStats mode key paths must be distinct", leftPath != rightPath);
            uassert(13176203,
                    str::stream() << "fieldStats mode key paths may not overlap: '"
                                  << leftPath.fullPath() << "' is a prefix of '"
                                  << rightPath.fullPath() << "'",
                    !leftPath.isPrefixOf(rightPath));
        }
    }
    return parsedPaths;
}

}  // namespace

FieldStatsBuilder::FieldStatsBuilder(const std::vector<std::string>& fieldPaths,
                                     const UUID& collectionUuid)
    : _parsedPaths(normalizeFieldStatsKeyPaths(fieldPaths)), _collectionUuid(collectionUuid) {}

void FieldStatsBuilder::accumulate(const BSONObj& document) {
    try {
        const auto [tuples, hasNonScalar] = stats::extractFieldValues(document, _parsedPaths);
        if (hasNonScalar) {
            ++_nonScalarDocs;
        } else {
            bool hasNull = false;
            bool hasMissing = false;
            bool hasUndefined = false;
            for (const auto& tuple : tuples) {
                for (const auto value : tuple) {
                    hasNull |= value.tag == sbe::value::TypeTags::Null;
                    hasMissing |= value.tag == sbe::value::TypeTags::Nothing;
                    hasUndefined |= value.tag == sbe::value::TypeTags::bsonUndefined;
                }
                _ndvBuilder.accumulate(tuple);
                _mcvBuilder.accumulate(tuple);
            }
            // Composite paths can share a category; count each document only once per category.
            _nullDocs += hasNull;
            _missingDocs += hasMissing;
            _undefinedDocs += hasUndefined;
        }
    } catch (const ExceptionFor<ErrorCodes::ExceededMemoryLimit>&) {
        // Oversized array products are excluded from these statistics anyway.
        ++_nonScalarDocs;
    }
    ++_numDocumentsObserved;
}

FieldStatsDoc FieldStatsBuilder::getStats() const {
    std::vector<std::string> fieldPaths;
    fieldPaths.reserve(_parsedPaths.size());
    for (const auto& path : _parsedPaths) {
        fieldPaths.push_back(path.fullPath());
    }

    FieldStatsDoc document;
    document.set_id(makeFieldStatsId(_collectionUuid, std::move(fieldPaths)));
    document.setSchemaVersion(kFieldStatsSchemaVersion);
    document.setCollectedAt(_collectedAt);
    document.setTotalDocs(_numDocumentsObserved);
    document.setNonScalarDocs(_nonScalarDocs);
    document.setNullDocs(_nullDocs);
    document.setMissingDocs(_missingDocs);
    document.setUndefinedDocs(_undefinedDocs);
    document.setNdv(_ndvBuilder.getStats());
    document.setMcv(_mcvBuilder.getStats());
    return document;
}

}  // namespace mongo::ce
