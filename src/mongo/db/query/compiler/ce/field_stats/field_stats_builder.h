// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/ce/field_stats/field_stats_gen.h"
#include "mongo/db/query/compiler/ce/field_stats/mcv_sketch_builder.h"
#include "mongo/db/query/compiler/ce/field_stats/ndv_sketch_builder.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mongo::ce {

/**
 * Collects composite NDV and independent per-field MCVs from the same document observations.
 * Excludes documents with arrays along or objects at any analyzed path from both statistics.
 * Validates and normalizes field paths before constructing the sketch builders.
 */
class FieldStatsBuilder {
public:
    explicit FieldStatsBuilder(const std::vector<std::string>& fieldPaths,
                               const UUID& collectionUuid);

    void accumulate(const BSONObj& document);

    FieldStatsDoc getStats() const;

private:
    const std::vector<FieldPath> _parsedPaths;
    const UUID _collectionUuid;
    const Date_t _collectedAt = Date_t::now();
    NdvSketchBuilder _ndvBuilder{_parsedPaths.size()};
    McvSketchBuilder _mcvBuilder{_parsedPaths.size()};
    std::int64_t _numDocumentsObserved = 0;
    std::int64_t _nonScalarDocs = 0;
    std::int64_t _nullDocs = 0;
    std::int64_t _missingDocs = 0;
    std::int64_t _undefinedDocs = 0;
};

}  // namespace mongo::ce
