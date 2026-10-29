// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/storage_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/optional/optional.hpp>

#ifdef _WIN32
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#endif
namespace mongo {

StorageGlobalParams::StorageGlobalParams() {
    _reset();
}

void StorageGlobalParams::reset_forTest() {
    _reset();
}

void StorageGlobalParams::_reset() {
    engine = "wiredTiger";
    engineSetByUser = false;
    dbpath = kDefaultDbPath;
    upgrade = false;
    repair = false;
    validate = false;
    restore = false;
    magicRestore = false;

    noTableScan.store(false);
    directoryperdb = false;
    syncdelay.store(kDefaultSyncDelaySecs);
    queryableBackupMode = false;
    groupCollections = false;
    oplogMinRetentionHours.store(0.0);
    oplogMinRetentionInitializedUsingDefault = true;
    allowOplogTruncation = true;
    forceDisableTableLogging = false;
    enableSpillEngine = true;
}

boost::filesystem::path StorageGlobalParams::getSpillDbPath() const {
    return gSpillPath.empty() ? (boost::filesystem::path(dbpath) / "_tmp" / "spilldb")
                              : boost::filesystem::path(gSpillPath);
}

StorageGlobalParams storageGlobalParams;

Status StorageDirectoryPerDbParameter::setFromString(std::string_view,
                                                     const boost::optional<TenantId>&) {
    return {ErrorCodes::IllegalOperation,
            str::stream() << name() << " cannot be set via setParameter"};
};

void StorageDirectoryPerDbParameter::append(OperationContext* opCtx,
                                            BSONObjBuilder* builder,
                                            std::string_view name,
                                            const boost::optional<TenantId>&) {
    builder->append(name, storageGlobalParams.directoryperdb);
}


/**
 * The directory where the mongod instance stores its data.
 */
#ifdef _WIN32
const char* StorageGlobalParams::kDefaultDbPath = "\\data\\db\\";
const char* StorageGlobalParams::kDefaultConfigDbPath = "\\data\\configdb\\";
#else
const char* StorageGlobalParams::kDefaultDbPath = "/data/db";
const char* StorageGlobalParams::kDefaultConfigDbPath = "/data/configdb";
#endif

std::string storageDBPathDescription() {
    StringBuilder sb;

    sb << "Directory for datafiles - defaults to " << storageGlobalParams.kDefaultDbPath;

#ifdef _WIN32
    boost::filesystem::path currentPath = boost::filesystem::current_path();

    sb << " which is " << currentPath.root_name().string() << storageGlobalParams.kDefaultDbPath
       << " based on the current working drive";
#endif

    return sb.str();
}

}  // namespace mongo
