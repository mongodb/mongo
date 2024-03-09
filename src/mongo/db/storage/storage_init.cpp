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

#include <memory>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"

namespace mongo {
namespace {

class StorageSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~StorageSSS() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        auto svcCtx = opCtx->getClient()->getServiceContext();
        auto engine = svcCtx->getStorageEngine();
        auto oldestRequiredTimestampForCrashRecovery = engine->getOplogNeededForCrashRecovery();
        auto backupCursorHooks = BackupCursorHooks::get(svcCtx);

        BSONObjBuilder bob;
        bob.append("name", storageGlobalParams.engine);
        bob.append("supportsCommittedReads", engine->supportsReadConcernMajority());
        bob.append("oldestRequiredTimestampForCrashRecovery",
                   oldestRequiredTimestampForCrashRecovery
                       ? *oldestRequiredTimestampForCrashRecovery
                       : Timestamp());
        bob.append("supportsPendingDrops", engine->supportsPendingDrops());
        bob.append("dropPendingIdents", static_cast<long long>(engine->getNumDropPendingIdents()));
        bob.append("supportsSnapshotReadConcern", engine->supportsReadConcernSnapshot());
        bob.append("readOnly", !opCtx->getServiceContext()->userWritesAllowed());
        bob.append("persistent", !engine->isEphemeral());
        bob.append("backupCursorOpen", backupCursorHooks->isBackupCursorOpen());

        return bob.obj();
    }
};
auto& storageSSS = *ServerStatusSectionBuilder<StorageSSS>("storageEngine").forShard();

}  // namespace
}  // namespace mongo
