// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/rwc_defaults_commands_gen.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults.h"

#include <memory>

namespace mongo {
namespace {

class ReadWriteConcernDefaultsServerStatus final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        auto rwcDefault = ReadWriteConcernDefaults::get(opCtx).getDefault(opCtx);
        GetDefaultRWConcernResponse response;
        response.setRWConcernDefault(rwcDefault);
        response.setLocalUpdateWallClockTime(rwcDefault.localUpdateWallClockTime());
        return response.toBSON();
    }
};
auto& defaultRWConcernServerStatus =
    *ServerStatusSectionBuilder<ReadWriteConcernDefaultsServerStatus>("defaultRWConcern")
         .forRouter();

}  // namespace
}  // namespace mongo
