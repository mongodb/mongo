// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/storage_engine_lock_file.h"

#include "mongo/platform/process_id.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <ostream>
#include <string_view>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {

auto getLockFile = ServiceContext::declareDecoration<boost::optional<StorageEngineLockFile>>();

}  // namespace

boost::optional<StorageEngineLockFile>& StorageEngineLockFile::get(ServiceContext* service) {
    return getLockFile(service);
}

std::string StorageEngineLockFile::lockFilePath(std::string_view dbpath,
                                                std::string_view fileName) {
    return (boost::filesystem::path(std::string{dbpath}) / std::string{fileName}).string();
}

Status StorageEngineLockFile::writePid() {
    ProcessId pid = ProcessId::getCurrent();
    std::stringstream ss;
    ss << pid << std::endl;
    std::string pidStr = ss.str();

    return writeString(pidStr);
}

std::string StorageEngineLockFile::_getNonExistentPathMessage() const {
    return str::stream() << "Data directory " << _dbpath
                         << " not found. Create the missing directory or specify another path "
                            "using (1) the --dbpath command line option, or (2) by adding the "
                            "'storage.dbPath' option in the configuration file.";
}

void StorageEngineLockFile::create(ServiceContext* service, std::string_view dbpath) {
    auto& lockFile = StorageEngineLockFile::get(service);
    try {
        lockFile.emplace(dbpath);
    } catch (const std::exception& ex) {
        uasserted(28596,
                  str::stream() << "Unable to determine status of lock file in the data directory "
                                << dbpath << ": " << ex.what());
    }
    const bool wasUnclean = lockFile->createdByUncleanShutdown();
    const auto openStatus = lockFile->open();
    if (openStatus == ErrorCodes::IllegalOperation) {
        lockFile = boost::none;
    } else {
        uassertStatusOK(openStatus);
    }

    if (wasUnclean) {
        LOGV2_WARNING(22271,
                      "Detected unclean shutdown - Lock file is not empty",
                      "lockFile"_attr = lockFile->getFilespec());
    }
}

}  // namespace mongo
