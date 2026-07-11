// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_stats_gen.h"
#include "mongo/db/session/session_catalog.h"

#include <cstdint>
#include <memory>

namespace mongo {
namespace {

class LogicalSessionServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        const auto logicalSessionCache = LogicalSessionCache::get(opCtx);
        const auto sessionCatalog = SessionCatalog::get(opCtx);

        BSONObjBuilder statsBuilder(logicalSessionCache ? logicalSessionCache->getStats().toBSON()
                                                        : BSONObj());
        statsBuilder.append("sessionCatalogSize", int32_t(sessionCatalog->size()));

        return statsBuilder.obj();
    }
};

auto& logicalSessionServerStatusSection =
    *ServerStatusSectionBuilder<LogicalSessionServerStatusSection>("logicalSessionRecordCache");

}  // namespace
}  // namespace mongo
