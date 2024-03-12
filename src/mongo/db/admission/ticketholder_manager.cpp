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

#include "mongo/db/admission/ticketholder_manager.h"

#include <utility>


#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/feature_flag.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/priority_ticketholder.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace admission {

namespace {
const auto ticketHolderManagerDecoration =
    mongo::ServiceContext::declareDecoration<std::unique_ptr<TicketHolderManager>>();
}

TicketHolderManager::TicketHolderManager(std::unique_ptr<TicketHolder> readTicketHolder,
                                         std::unique_ptr<TicketHolder> writeTicketHolder)
    : _readTicketHolder(std::move(readTicketHolder)),
      _writeTicketHolder(std::move(writeTicketHolder)) {}

Status TicketHolderManager::updateConcurrentWriteTransactions(const int32_t& newWriteTransactions) {
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

        if (!ticketHolderManager->supportsRuntimeSizeAdjustment()) {
            // In order to manually set the number of read/write tickets, users must set the
            // storageEngineConcurrencyAdjustmentAlgorithm to 'kFixedConcurrentTransactions'.
            return {
                ErrorCodes::IllegalOperation,
                "Cannot modify concurrent write transactions limit when it is being dynamically "
                "adjusted"};
        }

        auto& writer = ticketHolderManager->_writeTicketHolder;
        if (writer) {
            writer->resize(newWriteTransactions, Date_t::max());
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

        if (!ticketHolderManager->supportsRuntimeSizeAdjustment()) {
            // In order to manually set the number of read/write tickets, users must set the
            // storageEngineConcurrencyAdjustmentAlgorithm to 'kFixedConcurrentTransactions'.
            return {ErrorCodes::IllegalOperation,
                    "Cannot modify concurrent read transactions limit when it is being dynamically "
                    "adjusted"};
        }

        auto& reader = ticketHolderManager->_readTicketHolder;
        if (reader) {
            reader->resize(newReadTransactions, Date_t::max());
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

Status TicketHolderManager::validateConcurrentWriteTransactions(const int32_t& newWriteTransactions,
                                                                const boost::optional<TenantId>) {
    if (!getTestCommandsEnabled() && newWriteTransactions < 5) {
        return Status(ErrorCodes::BadValue,
                      "Concurrent write transactions limit must be greater than or equal to 5.");
    }
    return Status::OK();
}

Status TicketHolderManager::validateConcurrentReadTransactions(const int32_t& newReadTransactions,
                                                               const boost::optional<TenantId>) {
    if (!getTestCommandsEnabled() && newReadTransactions < 5) {
        return Status(ErrorCodes::BadValue,
                      "Concurrent read transactions limit must be greater than or equal to 5.");
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

    _appendImplStats(b);
}

}  // namespace admission
}  // namespace mongo
