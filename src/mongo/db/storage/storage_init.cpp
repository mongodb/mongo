// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class StorageSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~StorageSSS() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        return opCtx->getServiceContext()->getStorageEngine()->getStatus(opCtx);
    }
};
auto& storageSSS = *ServerStatusSectionBuilder<StorageSSS>("storageEngine").forShard();

}  // namespace
}  // namespace mongo
