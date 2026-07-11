// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/storage_repair_observer.h"

#include <algorithm>
#include <ostream>
#include <utility>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_file_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {
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

void StorageRepairObserver::onRepairDone(OperationContext* opCtx,
                                         const InvalidateReplConfigCallback& cb) {
    invariant(_repairState == RepairState::kIncomplete);

    // This ordering is important. The incomplete file should only be removed once the
    // replica set configuration has been invalidated successfully.
    if (isDataInvalidated()) {
        invariant(opCtx && cb);
        cb();
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
                            "Failed to remove file",
                            "file"_attr = _repairIncompleteFilePath.generic_string(),
                            "error"_attr = ec.message());
    }
    fassertNoTrace(50927, fsyncParentDirectory(_repairIncompleteFilePath));
}

}  // namespace mongo
