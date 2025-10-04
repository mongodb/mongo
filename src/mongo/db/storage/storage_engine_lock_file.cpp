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


#include "mongo/db/storage/storage_engine_lock_file.h"

#include "mongo/platform/process_id.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <ostream>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {

auto getLockFile = ServiceContext::declareDecoration<boost::optional<StorageEngineLockFile>>();

}  // namespace

boost::optional<StorageEngineLockFile>& StorageEngineLockFile::get(ServiceContext* service) {
    return getLockFile(service);
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

void StorageEngineLockFile::create(ServiceContext* service, StringData dbpath) {
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
