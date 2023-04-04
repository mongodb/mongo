/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/storage/ticketholder_manager.h"
#include "mongo/db/storage/execution_control/throughput_probing.h"
#include "mongo/db/storage/storage_engine_feature_flags_gen.h"
#include "mongo/db/storage/storage_engine_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/priority_ticketholder.h"
#include "mongo/util/concurrency/semaphore_ticketholder.h"
#include "mongo/util/concurrency/ticketholder.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace {
const auto ticketHolderManagerDecoration =
    mongo::ServiceContext::declareDecoration<std::unique_ptr<mongo::TicketHolderManager>>();
}

namespace mongo {

TicketHolderManager::TicketHolderManager(ServiceContext* svcCtx,
                                         std::unique_ptr<TicketHolder> readTicketHolder,
                                         std::unique_ptr<TicketHolder> writeTicketHolder)
    : _readTicketHolder(std::move(readTicketHolder)),
      _writeTicketHolder(std::move(writeTicketHolder)),
      _monitor([this, svcCtx]() -> std::unique_ptr<TicketHolderMonitor> {
          // (Ignore FCV check): This feature flag doesn't have upgrade/downgrade concern.
          if (!feature_flags::gFeatureFlagExecutionControl.isEnabledAndIgnoreFCVUnsafe()) {
              return nullptr;
          }
          switch (StorageEngineConcurrencyAdjustmentAlgorithm_parse(
              IDLParserContext{"storageEngineConcurrencyAdjustmentAlgorithm"},
              gStorageEngineConcurrencyAdjustmentAlgorithm)) {
              case StorageEngineConcurrencyAdjustmentAlgorithmEnum::kNone:
                  return nullptr;
              case StorageEngineConcurrencyAdjustmentAlgorithmEnum::kThroughputProbing:
                  return std::make_unique<execution_control::ThroughputProbing>(
                      svcCtx, _readTicketHolder.get(), _writeTicketHolder.get(), Milliseconds{100});
          }
          MONGO_UNREACHABLE;
      }()) {
    if (_monitor) {
        _monitor->start();
    }
}

Status TicketHolderManager::updateConcurrentWriteTransactions(const int32_t& newWriteTransactions) {
    // (Ignore FCV check): This feature flag doesn't have upgrade/downgrade concern.
    if (feature_flags::gFeatureFlagExecutionControl.isEnabledAndIgnoreFCVUnsafe() &&
        !gStorageEngineConcurrencyAdjustmentAlgorithm.empty()) {
        return {ErrorCodes::IllegalOperation,
                "Cannot modify concurrent write transactions limit when it is being dynamically "
                "adjusted"};
    }

    if (auto client = Client::getCurrent()) {
        auto ticketHolderManager = TicketHolderManager::get(client->getServiceContext());
        if (!ticketHolderManager) {
            LOGV2_WARNING(7323602,
                          "Attempting to modify write transactions limit on an instance without a "
                          "storage engine");
            return Status(ErrorCodes::IllegalOperation,
                          "Attempting to modify write transactions limit on an instance without a "
                          "storage engine");
        }
        auto& writer = ticketHolderManager->_writeTicketHolder;
        if (writer) {
            writer->resize(newWriteTransactions);
            return Status::OK();
        }
        LOGV2_WARNING(6754202,
                      "Attempting to update concurrent write transactions limit before the "
                      "write TicketHolder is initialized");
        return Status(ErrorCodes::IllegalOperation,
                      "Attempting to update concurrent write transactions limit before the "
                      "write TicketHolder is initialized");
    }
    return Status::OK();
};

Status TicketHolderManager::updateConcurrentReadTransactions(const int32_t& newReadTransactions) {
    // (Ignore FCV check): This feature flag doesn't have upgrade/downgrade concern.
    if (feature_flags::gFeatureFlagExecutionControl.isEnabledAndIgnoreFCVUnsafe() &&
        !gStorageEngineConcurrencyAdjustmentAlgorithm.empty()) {
        return {ErrorCodes::IllegalOperation,
                "Cannot modify concurrent read transactions limit when it is being dynamically "
                "adjusted"};
    }

    if (auto client = Client::getCurrent()) {
        auto ticketHolderManager = TicketHolderManager::get(client->getServiceContext());
        if (!ticketHolderManager) {
            LOGV2_WARNING(7323601,
                          "Attempting to modify read transactions limit on an instance without a "
                          "storage engine");
            return Status(ErrorCodes::IllegalOperation,
                          "Attempting to modify read transactions limit on an instance without a "
                          "storage engine");
        }
        auto& reader = ticketHolderManager->_readTicketHolder;
        if (reader) {
            reader->resize(newReadTransactions);
            return Status::OK();
        }

        LOGV2_WARNING(6754201,
                      "Attempting to update concurrent read transactions limit before the read "
                      "TicketHolder is initialized");
        return Status(ErrorCodes::IllegalOperation,
                      "Attempting to update concurrent read transactions limit before the read "
                      "TicketHolder is initialized");
    }
    return Status::OK();
}

Status TicketHolderManager::updateLowPriorityAdmissionBypassThreshold(
    const int32_t& newBypassThreshold) {
    if (auto client = Client::getCurrent()) {
        // TODO SERVER-72616: Remove the ifdef once TicketPool is implemented in a cross-platform
        // manner.
#ifdef __linux__
        auto ticketHolderManager = TicketHolderManager::get(client->getServiceContext());
        auto reader = dynamic_cast<PriorityTicketHolder*>(
            ticketHolderManager->getTicketHolder(LockMode::MODE_IS));
        auto writer = dynamic_cast<PriorityTicketHolder*>(
            ticketHolderManager->getTicketHolder(LockMode::MODE_IX));

        if (reader && writer) {
            reader->updateLowPriorityAdmissionBypassThreshold(newBypassThreshold);
            writer->updateLowPriorityAdmissionBypassThreshold(newBypassThreshold);
            return Status::OK();
        }

        // The 'lowPriorityAdmissionBypassThreshold' only impacts PriorityTicketHolders.
        LOGV2_WARNING(7092700,
                      "Attempting to update lowPriorityAdmissionBypassThreshold when the "
                      "Ticketholders are not initalized to be PriorityTicketholders");
        return Status(ErrorCodes::IllegalOperation,
                      "Attempting to update lowPriorityAdmissionBypassThreshold when the "
                      "TicketHolders are not initalized to be PriorityTicketholders");
#else
        LOGV2_WARNING(7207204,
                      "Attempting to update lowPriorityAdmissionBypassThreshold when the feature "
                      "is only supported on Linux");
        return Status(ErrorCodes::IllegalOperation,
                      "Attempting to update lowPriorityAdmissionBypassThreshold when the feature "
                      "is only supported on Linux");
#endif
    }
    return Status::OK();
}

TicketHolderManager* TicketHolderManager::get(ServiceContext* svcCtx) {
    return ticketHolderManagerDecoration(svcCtx).get();
}

void TicketHolderManager::use(ServiceContext* svcCtx,
                              std::unique_ptr<TicketHolderManager> newTicketHolderManager) {
    ticketHolderManagerDecoration(svcCtx) = std::move(newTicketHolderManager);
}

TicketHolder* TicketHolderManager::getTicketHolder(LockMode mode) {
    auto readerTicketHolder = _readTicketHolder.get();
    auto writerTicketHolder = _writeTicketHolder.get();
    if (!(readerTicketHolder && writerTicketHolder)) {
        return nullptr;
    }
    switch (mode) {
        case MODE_S:
        case MODE_IS:
            return readerTicketHolder;
        case MODE_IX:
            return writerTicketHolder;
        default:
            return nullptr;
    }
}

void TicketHolderManager::appendStats(BSONObjBuilder& b) {
    invariant(_writeTicketHolder, "Writer TicketHolder is not present in the TicketHolderManager");
    invariant(_readTicketHolder, "Reader TicketHolder is not present in the TicketHolderManager");
    {
        BSONObjBuilder bbb(b.subobjStart("write"));
        _writeTicketHolder->appendStats(bbb);
        bbb.done();
    }
    {
        BSONObjBuilder bbb(b.subobjStart("read"));
        _readTicketHolder->appendStats(bbb);
        bbb.done();
    }
    if (_monitor) {
        BSONObjBuilder bbb(b.subobjStart("monitor"));
        _monitor->appendStats(bbb);
        bbb.done();
    }
}

Status TicketHolderManager::validateConcurrencyAdjustmentAlgorithm(
    const std::string& name, const boost::optional<TenantId>&) try {
    StorageEngineConcurrencyAdjustmentAlgorithm_parse(
        IDLParserContext{"storageEngineConcurrencyAdjustmentAlgorithm"}, name);
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}
}  // namespace mongo
