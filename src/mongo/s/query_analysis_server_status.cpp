// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/query_analysis_sample_tracker.h"

#include <memory>

namespace mongo {
namespace analyze_shard_key {
namespace {

class QueryAnalysisServerStatus final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return supportsSamplingQueries(getGlobalServiceContext());
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        return supportsSamplingQueries(opCtx)
            ? QueryAnalysisSampleTracker::get(opCtx).reportForServerStatus()
            : BSONObj();
    }
};
auto queryAnalysisServerStatus =
    *ServerStatusSectionBuilder<QueryAnalysisServerStatus>("queryAnalyzers");

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
