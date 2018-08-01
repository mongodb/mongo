/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/storage_repair_observer.h"

#include <cerrno>
#include <cstring>

#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <boost/filesystem/path.hpp>

#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/file.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
static const NamespaceString kConfigNss("local.system.replset");
static const std::string kRepairIncompleteFileName = "_repair_incomplete";

const auto getRepairObserver =
    ServiceContext::declareDecoration<std::unique_ptr<StorageRepairObserver>>();

Status fsyncFile(const boost::filesystem::path& path) {
    File file;
    file.open(path.string().c_str(), /*read-only*/ false, /*direct-io*/ false);
    if (!file.is_open()) {
        return {ErrorCodes::FileOpenFailed,
                str::stream() << "Failed to open file " << path.string()};
    }
    file.fsync();
    return Status::OK();
}

Status fsyncParentDirectory(const boost::filesystem::path& file) {
#ifdef __linux__  // this isn't needed elsewhere
    if (!file.has_parent_path()) {
        return {ErrorCodes::InvalidPath,
                str::stream() << "Couldn't find parent directory for file: " << file.string()};
    }

    boost::filesystem::path dir = file.parent_path();

    LOG(1) << "flushing directory " << dir.string();

    int fd = ::open(dir.string().c_str(), O_RDONLY);
    if (fd < 0) {
        return {ErrorCodes::FileOpenFailed,
                str::stream() << "Failed to open directory " << dir.string() << " for flushing: "
                              << errnoWithDescription()};
    }
    if (fsync(fd) != 0) {
        int e = errno;
        if (e == EINVAL) {
            warning() << "Could not fsync directory because this file system is not supported.";
        } else {
            close(fd);
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Failed to fsync directory '" << dir.string() << "': "
                                  << errnoWithDescription(e)};
        }
    }
    close(fd);
#endif
    return Status::OK();
}

}  // namespace

StorageRepairObserver::StorageRepairObserver(const std::string& dbpath) {
    invariant(!storageGlobalParams.readOnly);

    using boost::filesystem::path;
    _repairIncompleteFilePath = path(dbpath) / path(kRepairIncompleteFileName);

    _repairState = boost::filesystem::exists(_repairIncompleteFilePath) ? RepairState::kIncomplete
                                                                        : RepairState::kPreStart;
}

StorageRepairObserver* StorageRepairObserver::get(ServiceContext* service) {
    return getRepairObserver(service).get();
}

void StorageRepairObserver::set(ServiceContext* service,
                                std::unique_ptr<StorageRepairObserver> repairObserver) {
    auto& manager = getRepairObserver(service);
    manager = std::move(repairObserver);
}

void StorageRepairObserver::onRepairStarted() {
    invariant(_repairState == RepairState::kPreStart || _repairState == RepairState::kIncomplete);
    _touchRepairIncompleteFile();
    _repairState = RepairState::kIncomplete;
}

void StorageRepairObserver::onModification(const std::string& description) {
    invariant(_repairState == RepairState::kIncomplete);
    _modifications.emplace_back(description);
}

void StorageRepairObserver::onRepairDone(OperationContext* opCtx) {
    invariant(_repairState == RepairState::kIncomplete);

    // This ordering is important. The incomplete file should only be removed once the
    // replica set configuration has been invalidated successfully.
    if (!_modifications.empty()) {
        _invalidateReplConfigIfNeeded(opCtx);
    }
    _removeRepairIncompleteFile();

    _repairState = RepairState::kDone;
}

void StorageRepairObserver::_touchRepairIncompleteFile() {
    boost::filesystem::ofstream fileStream(_repairIncompleteFilePath);
    fileStream << "This file indicates that a repair operation is in progress or incomplete.";
    if (fileStream.fail()) {
        severe() << "Failed to write to file " << _repairIncompleteFilePath.string() << ": "
                 << errnoWithDescription();
        fassertFailedNoTrace(50920);
    }
    fileStream.close();

    fassertNoTrace(50924, fsyncFile(_repairIncompleteFilePath));
    fassertNoTrace(50925, fsyncParentDirectory(_repairIncompleteFilePath));
}

void StorageRepairObserver::_removeRepairIncompleteFile() {
    boost::system::error_code ec;
    boost::filesystem::remove(_repairIncompleteFilePath, ec);

    if (ec) {
        severe() << "Failed to remove file " << _repairIncompleteFilePath.string() << ": "
                 << ec.message();
        fassertFailedNoTrace(50921);
    }
    fassertNoTrace(50927, fsyncParentDirectory(_repairIncompleteFilePath));
}

void StorageRepairObserver::_invalidateReplConfigIfNeeded(OperationContext* opCtx) {
    // If the config doesn't exist, don't invalidate anything. If this node were originally part of
    // a replica set but lost its config due to a repair, it would automatically perform a resync.
    // If this node is a standalone, this would lead to a confusing error message if it were
    // added to a replica set later on.
    BSONObj config;
    if (!Helpers::getSingleton(opCtx, kConfigNss.ns().c_str(), config)) {
        return;
    }
    if (config.hasField(repl::ReplSetConfig::kRepairedFieldName)) {
        return;
    }
    BSONObjBuilder configBuilder(config);
    configBuilder.append(repl::ReplSetConfig::kRepairedFieldName, true);
    Helpers::putSingleton(opCtx, kConfigNss.ns().c_str(), configBuilder.obj());

    opCtx->recoveryUnit()->waitUntilDurable();
}

}  // namespace mongo
