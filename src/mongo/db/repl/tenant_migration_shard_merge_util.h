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

#include <boost/filesystem/path.hpp>
#include <fmt/format.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_server_params_gen.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

namespace mongo::repl::shard_merge_utils {

inline constexpr StringData kDonatedFilesPrefix = "donatedFiles."_sd;
inline constexpr StringData kImportDoneMarkerPrefix = "importDoneMarker."_sd;
inline constexpr StringData kMigrationTmpDirPrefix = "migrationTmpFiles"_sd;
inline constexpr StringData kMigrationIdFieldName = "migrationId"_sd;
inline constexpr StringData kBackupIdFieldName = "backupId"_sd;
inline constexpr StringData kDonorHostNameFieldName = "donorHostName"_sd;
inline constexpr StringData kDonorDbPathFieldName = "dbpath"_sd;
inline constexpr StringData kTableExtension = ".wt"_sd;

// Keep the backup cursor alive by pinging twice as often as the donor's default
// cursor timeout.
constexpr int kBackupCursorKeepAliveIntervalMillis = mongo::kCursorTimeoutMillisDefault / 2;

inline bool isDonatedFilesCollection(const NamespaceString& ns) {
    return ns.isConfigDB() && ns.coll().startsWith(kDonatedFilesPrefix);
}

inline NamespaceString getDonatedFilesNs(const UUID& migrationUUID) {
    return NamespaceString::makeGlobalConfigCollection(kDonatedFilesPrefix +
                                                       migrationUUID.toString());
}

inline NamespaceString getImportDoneMarkerNs(const UUID& migrationUUID) {
    return NamespaceString::makeLocalCollection(kImportDoneMarkerPrefix + migrationUUID.toString());
}

inline boost::filesystem::path fileClonerTempDir(const UUID& migrationId) {
    return boost::filesystem::path(storageGlobalParams.dbpath) /
        fmt::format("{}.{}", kMigrationTmpDirPrefix.toString(), migrationId.toString());
}

/**
 * Computes a boost::filesystem::path generic-style relative path (always uses slashes)
 * from a base path and a relative path.
 */
std::string getPathRelativeTo(const std::string& path, const std::string& basePath);

/**
 * Represents the document structure of config.donatedFiles_<MigrationUUID> collection.
 */
struct MetadataInfo {
    explicit MetadataInfo(const UUID& backupId,
                          const UUID& migrationId,
                          const std::string& donorHostAndPort,
                          const std::string& donorDbPath)
        : backupId(backupId),
          migrationId(migrationId),
          donorHostAndPort(donorHostAndPort),
          donorDbPath(donorDbPath) {}
    UUID backupId;
    UUID migrationId;
    std::string donorHostAndPort;
    std::string donorDbPath;

    static MetadataInfo constructMetadataInfo(const UUID& migrationId,
                                              const std::string& donorHostAndPort,
                                              const BSONObj& obj) {
        auto backupId = UUID(uassertStatusOK(UUID::parse(obj[kBackupIdFieldName])));
        auto donorDbPath = obj[kDonorDbPathFieldName].String();
        return MetadataInfo{backupId, migrationId, donorHostAndPort, donorDbPath};
    }

    BSONObj toBSON(const BSONObj& extraFields) const {
        BSONObjBuilder bob;

        migrationId.appendToBuilder(&bob, kMigrationIdFieldName);
        backupId.appendToBuilder(&bob, kBackupIdFieldName);
        bob.append(kDonorHostNameFieldName, donorHostAndPort);
        bob.append(kDonorDbPathFieldName, donorDbPath);
        bob.append("_id", OID::gen());
        bob.appendElements(extraFields);

        return bob.obj();
    }
};

/**
 * Helpers to create and drop the import done marker collection.
 */
void createImportDoneMarkerLocalCollection(OperationContext* opCtx, const UUID& migrationId);
void dropImportDoneMarkerLocalCollection(OperationContext* opCtx, const UUID& migrationId);

/**
 * Checks if the import done marker collection exists; triggers a fatal assertion if absent.
 *
 * Note: Call this method only during shard merge recipient state transition to kConsistent.
 */
void assertImportDoneMarkerLocalCollExistsOnMergeConsistent(OperationContext* opCtx,
                                                            const UUID& migrationId);

/**
 * Reads a list of filenames from the marker file kMovingFilesMarker.
 * Returns empty vector if the marker file is not present.
 */
std::vector<std::string> readMovingFilesMarker(const boost::filesystem::path& markerDir);

/**
 * Writes out a marker file kMovingFilesMarker. And, flushes the marker file and it's parent
 * directory.
 * @param 'firstEntry' determines whether to truncate the file before writing data or to append the
 * data.
 */
void writeMovingFilesMarker(const boost::filesystem::path& markerDir,
                            const std::string& ident,
                            bool firstEntry);

/**
 * Returns a path by appending the given ident to the srcPath.
 */
boost::filesystem::path constructSourcePath(const boost::filesystem::path& srcPath,
                                            const std::string& ident);

/**
 * Returns a path by appending the given ident to the data directory path i.e,
 * storageGlobalParams.dbpath.
 */
boost::filesystem::path constructDestinationPath(const std::string& ident);

/**
 * Removes the file associated with the given path. Throws exceptions on failures.
 */
void removeFile(const boost::filesystem::path& path);

/**
 * performs fsync on the the data directory i.e, storageGlobalParams.dbpath.
 */
void fsyncDataDirectory();

/**
 * Removes the directory associated with the given path. And, performs fsync on the the parent
 * directory of 'path'. Throws exceptions on failures.
 */
void fsyncRemoveDirectory(const boost::filesystem::path& path);
}  // namespace mongo::repl::shard_merge_utils
