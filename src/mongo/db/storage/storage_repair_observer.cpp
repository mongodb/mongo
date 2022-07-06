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


#include "mongo/db/storage/storage_repair_observer.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/path.hpp>

#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/storage_file_util.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/file.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {
static const NamespaceString kConfigNss("local.system.replset");
static const std::string kRepairIncompleteFileName = "_repair_incomplete";

const auto getRepairObserver =
    ServiceContext::declareDecoration<std::unique_ptr<StorageRepairObserver>>();

}  // namespace

StorageRepairObserver::StorageRepairObserver(const std::string& dbpath) {
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

void StorageRepairObserver::benignModification(const std::string& description) {
    invariant(_repairState == RepairState::kIncomplete);
    _modifications.emplace_back(Modification::benign(description));
}

void StorageRepairObserver::invalidatingModification(const std::string& description) {
    invariant(_repairState == RepairState::kIncomplete);
    _modifications.emplace_back(Modification::invalidating(description));
}

void StorageRepairObserver::onRepairDone(OperationContext* opCtx) {
    invariant(_repairState == RepairState::kIncomplete);

    // This ordering is important. The incomplete file should only be removed once the
    // replica set configuration has been invalidated successfully.
    if (isDataInvalidated()) {
        _invalidateReplConfigIfNeeded(opCtx);
    }
    _removeRepairIncompleteFile();

    _repairState = RepairState::kDone;
}

bool StorageRepairObserver::isDataInvalidated() const {
    invariant(_repairState == RepairState::kIncomplete || _repairState == RepairState::kDone);
    return std::any_of(_modifications.begin(), _modifications.end(), [](Modification mod) -> bool {
        return mod.isInvalidating();
    });
}

void StorageRepairObserver::_touchRepairIncompleteFile() {
    boost::filesystem::ofstream fileStream(_repairIncompleteFilePath);
    fileStream << "This file indicates that a repair operation is in progress or incomplete.";
    if (fileStream.fail()) {
        auto ec = lastSystemError();
        LOGV2_FATAL_NOTRACE(50920,
                            "Failed to write to file {file}: {error}",
                            "Failed to write to file",
                            "file"_attr = _repairIncompleteFilePath.generic_string(),
                            "error"_attr = errorMessage(ec));
    }
    fileStream.close();

    fassertNoTrace(50924, fsyncFile(_repairIncompleteFilePath));
    fassertNoTrace(50925, fsyncParentDirectory(_repairIncompleteFilePath));
}

void StorageRepairObserver::_removeRepairIncompleteFile() {
    boost::system::error_code ec;
    boost::filesystem::remove(_repairIncompleteFilePath, ec);

    if (ec) {
        LOGV2_FATAL_NOTRACE(50921,
                            "Failed to remove file {file}: {error}",
                            "Failed to remove file",
                            "file"_attr = _repairIncompleteFilePath.generic_string(),
                            "error"_attr = ec.message());
    }
    fassertNoTrace(50927, fsyncParentDirectory(_repairIncompleteFilePath));
}

void StorageRepairObserver::_invalidateReplConfigIfNeeded(OperationContext* opCtx) {
    // If the config doesn't exist, don't invalidate anything. If this node were originally part of
    // a replica set but lost its config due to a repair, it would automatically perform a resync.
    // If this node is a standalone, this would lead to a confusing error message if it were
    // added to a replica set later on.
    BSONObj config;
    if (!Helpers::getSingleton(opCtx, kConfigNss, config)) {
        return;
    }
    if (config.hasField(repl::ReplSetConfig::kRepairedFieldName)) {
        return;
    }
    BSONObjBuilder configBuilder(config);
    configBuilder.append(repl::ReplSetConfig::kRepairedFieldName, true);
    Helpers::putSingleton(opCtx, kConfigNss, configBuilder.obj());

    JournalFlusher::get(opCtx)->waitForJournalFlush();
}

}  // namespace mongo
