// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/field_stats/field_stats_builder.h"

#include "mongo/db/query/compiler/ce/field_stats/field_stats.h"
#include "mongo/db/query/compiler/stats/field_values.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
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
        if (!hasNonScalar) {
            for (const auto& tuple : tuples) {
                _ndvBuilder.accumulate(tuple);
                _mcvBuilder.accumulate(tuple);
            }
        }
    } catch (const ExceptionFor<ErrorCodes::ExceededMemoryLimit>&) {
        // Oversized array products are excluded from these statistics anyway.
    }
    ++_numDocumentsObserved;
}

size_t FieldStatsBuilder::getNumDocumentsObserved() const {
    return _numDocumentsObserved;
}

FieldStatsDoc FieldStatsBuilder::getStats() const {
    std::vector<std::string> fieldPaths;
    fieldPaths.reserve(_parsedPaths.size());
    for (const auto& path : _parsedPaths) {
        fieldPaths.push_back(path.fullPath());
    }

    FieldStatsDoc document;
    document.set_id(makeFieldStatsId(_collectionUuid, fieldPaths));
    document.setSchemaVersion(kFieldStatsSchemaVersion);
    document.setCollectionUuid(_collectionUuid);
    document.setSortedFieldPaths(std::move(fieldPaths));
    document.setCreatedAt(_createdAt);
    document.setNdv(_ndvBuilder.getStats());
    document.setMcv(_mcvBuilder.getStats());
    return document;
}

}  // namespace mongo::ce
