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


#include "mongo/db/repl/tenant_migration_shard_merge_util.h"

#include <boost/filesystem/fstream.hpp>

#include "mongo/base/status.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/client_cursor/cursor_server_params_gen.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/storage_file_util.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

namespace mongo::repl::shard_merge_utils {
using namespace fmt::literals;
void createImportDoneMarkerLocalCollection(OperationContext* opCtx, const UUID& migrationId) {
    LOGV2_DEBUG(
        7458505, 1, "Creating import done marker collection", "migrationId"_attr = migrationId);

    UnreplicatedWritesBlock writeBlock(opCtx);
    // Collections in 'local' db should not expect any lock or prepare conflicts.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowAcquisitionOfLocks(
        shard_role_details::getLocker(opCtx));

    auto status = StorageInterface::get(opCtx)->createCollection(
        opCtx, getImportDoneMarkerNs(migrationId), CollectionOptions());

    if (!status.isOK()) {
        uassertStatusOK(status.withContext(
            str::stream() << "Failed to create import done marker local collection for migration: "
                          << migrationId));
    }
}

void dropImportDoneMarkerLocalCollection(OperationContext* opCtx, const UUID& migrationId) {
    LOGV2_DEBUG(
        7458506, 1, "Dropping import done marker collection", "migrationId"_attr = migrationId);

    UnreplicatedWritesBlock writeBlock(opCtx);
    // Collections in 'local' db should not expect any lock or prepare conflicts.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowAcquisitionOfLocks(
        shard_role_details::getLocker(opCtx));

    auto status =
        StorageInterface::get(opCtx)->dropCollection(opCtx, getImportDoneMarkerNs(migrationId));

    if (!status.isOK()) {
        uassertStatusOK(status.withContext(
            str::stream() << "Failed to drop import done marker local collection for migration: "
                          << migrationId));
    }
}

void assertImportDoneMarkerLocalCollExistsOnMergeConsistent(OperationContext* opCtx,
                                                            const UUID& migrationId) {
    const auto& markerNss = getImportDoneMarkerNs(migrationId);
    AllowLockAcquisitionOnTimestampedUnitOfWork allowAcquisitionOfLocks(
        shard_role_details::getLocker(opCtx));
    AutoGetCollectionForRead collection(opCtx, markerNss);

    // If the node is restored using cloud provider snapshot that was taken from a backup node
    // that's in the middle of of file copy/import phase of shard merge, it can cause the restored
    // node to have only partial donor data. And, if this node is restored (i.e resync) after it has
    // voted yes to R primary that it has imported all donor data, it can make R primary to commit
    // the migration and leading to have permanent data loss on this node. To prevent such
    // situation, we check the marker collection exists on transitioning to 'kConsistent'state to
    // guarantee that this node has imported all donor data.
    if (!collection) {
        LOGV2_FATAL_NOTRACE(
            7219902,
            "Shard merge trying to transition to 'kConsistent' state without 'ImportDoneMarker' "
            "collection. It's unsafe to continue at this point as there is no guarantee "
            "this node has copied all donor data. So, terminating this node.",
            "migrationId"_attr = migrationId,
            "markerNss"_attr = markerNss);
    }
}

std::vector<std::string> readMovingFilesMarker(const boost::filesystem::path& markerDir) {
    boost::filesystem::path markerPath(markerDir);
    markerPath.append(kMovingFilesMarker.toString());

    // The format of the marker files is simply strings of relative paths terminated with NULL
    // characters. This avoids having to worry about the BSON 16MB limit.
    std::vector<std::string> result;
    if (!boost::filesystem::exists(markerPath))
        return result;

    auto fileSize = boost::filesystem::file_size(markerPath);
    std::vector<char> contents(fileSize);

    boost::filesystem::ifstream reader(markerPath, std::ios_base::in | std::ios_base::binary);
    reader.read(contents.data(), fileSize);
    reader.close();

    auto startIter = contents.begin();
    auto nullIter = std::find(startIter, contents.end(), '\0');
    while (nullIter != contents.end()) {
        auto& nextFileName = result.emplace_back();
        std::copy(startIter, nullIter, std::back_inserter(nextFileName));
        startIter = nullIter + 1;
        nullIter = std::find(startIter, contents.end(), '\0');
    }
    return result;
}

void writeMovingFilesMarker(const boost::filesystem::path& markerDir,
                            const std::string& ident,
                            bool firstEntry) {
    uassert(ErrorCodes::NonExistentPath,
            str::stream() << "Marker's parent directory missing: " << markerDir.string(),
            boost::filesystem::exists(markerDir));
    boost::filesystem::path markerPath(markerDir);
    markerPath.append(kMovingFilesMarker.toString());

    auto mode = std::ios_base::out | std::ios_base::binary;
    if (firstEntry)
        mode |= std::ios_base::trunc;
    else
        mode |= std::ios::app;

    boost::filesystem::ofstream writer(markerPath, mode);

    writer.write(ident.c_str(), ident.size() + 1);

    writer.close();

    uassertStatusOK(
        fsyncFile(markerPath)
            .withContext(str::stream() << "failed to fsync marker file: " << markerPath.string()));
    uassertStatusOK(fsyncParentDirectory(markerPath));
}

boost::filesystem::path constructSourcePath(const boost::filesystem::path& srcPath,
                                            const std::string& ident) {
    boost::filesystem::path filePath{srcPath};
    filePath /= (ident + kTableExtension);
    return filePath;
}

boost::filesystem::path constructDestinationPath(const std::string& ident) {
    boost::filesystem::path filePath{storageGlobalParams.dbpath};
    filePath /= (ident + kTableExtension);
    return filePath;
}

void removeFile(const boost::filesystem::path& path) {
    LOGV2_DEBUG(7458501, 1, "Removing file", "path"_attr = path.string());

    boost::system::error_code ec;
    (void)boost::filesystem::remove(path, ec);
    if (ec) {
        uasserted(7458502, "Error removing file: '{}': {}"_format(path.string(), ec.message()));
    }
}

void fsyncDataDirectory() {
    boost::filesystem::path filePath{storageGlobalParams.dbpath};
    filePath /= "dummy";
    uassertStatusOK(fsyncParentDirectory(filePath));
}

void fsyncRemoveDirectory(const boost::filesystem::path& path) {
    LOGV2_DEBUG(7458503, 1, "Removing Directory", "path"_attr = path.string());

    boost::system::error_code ec;
    (void)boost::filesystem::remove_all(path, ec);
    if (ec) {
        uasserted(7458504,
                  "Error removing directory: '{}': {}"_format(path.string(), ec.message()));
    }
    uassertStatusOK(fsyncParentDirectory(path));
}
}  // namespace mongo::repl::shard_merge_utils
