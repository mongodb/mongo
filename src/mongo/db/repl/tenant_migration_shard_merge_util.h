/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <string>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <fmt/format.h>

#include "mongo/client/dbclient_connection.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/tenant_migration_shared_data.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_import.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo::repl::shard_merge_utils {

inline constexpr StringData kDonatedFilesPrefix = "donatedFiles."_sd;
inline constexpr StringData kMigrationTmpDirPrefix = "migrationTmpFiles"_sd;
inline constexpr StringData kMigrationIdFieldName = "migrationId"_sd;
inline constexpr StringData kBackupIdFieldName = "backupId"_sd;
inline constexpr StringData kDonorFieldName = "donor"_sd;
inline constexpr StringData kDonorDbPathFieldName = "dbpath"_sd;

inline bool isDonatedFilesCollection(const NamespaceString& ns) {
    return ns.isConfigDB() && ns.coll().startsWith(kDonatedFilesPrefix);
}

inline NamespaceString getDonatedFilesNs(const UUID& migrationUUID) {
    return NamespaceString(NamespaceString::kConfigDb,
                           kDonatedFilesPrefix + migrationUUID.toString());
}

inline boost::filesystem::path fileClonerTempDir(const UUID& migrationId) {
    return boost::filesystem::path(storageGlobalParams.dbpath) /
        fmt::format("{}.{}", kMigrationTmpDirPrefix.toString(), migrationId.toString());
}

/**
 * Represents the document structure of config.donatedFiles_<MigrationUUID> collection.
 */
struct MetadataInfo {
    explicit MetadataInfo(const UUID& backupId,
                          const UUID& migrationId,
                          const std::string& donor,
                          const std::string& donorDbPath)
        : backupId(backupId), migrationId(migrationId), donor(donor), donorDbPath(donorDbPath) {}
    UUID backupId;
    UUID migrationId;
    std::string donor;
    std::string donorDbPath;

    static MetadataInfo constructMetadataInfo(const UUID& migrationId,
                                              const std::string& donor,
                                              const BSONObj& obj) {
        auto backupId = UUID(uassertStatusOK(UUID::parse(obj[kBackupIdFieldName])));
        auto donorDbPath = obj[kDonorDbPathFieldName].String();
        return MetadataInfo{backupId, migrationId, donor, donorDbPath};
    }

    BSONObj toBSON(const BSONObj& extraFields) const {
        BSONObjBuilder bob;

        migrationId.appendToBuilder(&bob, kMigrationIdFieldName);
        backupId.appendToBuilder(&bob, kBackupIdFieldName);
        bob.append(kDonorFieldName, donor);
        bob.append(kDonorDbPathFieldName, donorDbPath);
        bob.append("_id", OID::gen());
        bob.appendElements(extraFields);

        return bob.obj();
    }
};

/**
 * Copy a file from the donor.
 */
void cloneFile(OperationContext* opCtx,
               DBClientConnection* clientConnection,
               ThreadPool* writerPool,
               TenantMigrationSharedData* sharedData,
               const BSONObj& metadataDoc);

/**
 * Import a donor collection after its files have been cloned to a temp dir.
 */
void wiredTigerImportFromBackupCursor(OperationContext* opCtx,
                                      const std::vector<CollectionImportMetadata>& metadatas,
                                      const std::string& importPath);

/**
 * Send a "getMore" to keep a backup cursor from timing out.
 */
SemiFuture<void> keepBackupCursorAlive(CancellationSource cancellationSource,
                                       std::shared_ptr<executor::TaskExecutor> executor,
                                       HostAndPort hostAndPort,
                                       CursorId cursorId,
                                       NamespaceString namespaceString);
}  // namespace mongo::repl::shard_merge_utils
