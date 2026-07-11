// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/vector_search_stats_entry.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::query_stats {

void VectorSearchStatsEntry::appendTo(BSONObjBuilder& builder) const {
    BSONObjBuilder metricsEntryBuilder = builder.subobjStart("vectorSearch");
    limit.appendTo(metricsEntryBuilder, "limit");
    numCandidatesLimitRatio.appendTo(metricsEntryBuilder, "numCandidatesLimitRatio");
}

void VectorSearchStatsEntry::updateStats(const SupplementalStatsEntry* other) {
    const VectorSearchStatsEntry* updateVal = dynamic_cast<const VectorSearchStatsEntry*>(other);
    tassert(9310800, "Unexpected type of statistic metric", updateVal != nullptr);
    limit.combine(updateVal->limit);
    numCandidatesLimitRatio.combine(updateVal->numCandidatesLimitRatio);
}

std::unique_ptr<SupplementalStatsEntry> VectorSearchStatsEntry::clone() const {
    return std::make_unique<VectorSearchStatsEntry>(*this);
}
}  // namespace mongo::query_stats
