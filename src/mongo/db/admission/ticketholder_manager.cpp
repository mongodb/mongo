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

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/decorable.h"

#include <utility>

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

Status TicketHolderManager::updateWriteMaxQueueDepth(std::int32_t newWriteMaxQueueDepth) {
    if (auto const client = Client::getCurrent()) {
        auto const ticketHolderManager = TicketHolderManager::get(client->getServiceContext());
        if (!ticketHolderManager) {
            constexpr auto& message =
                "Attempting to modify write max queue depth on an instance without a "
                "storage engine";
            LOGV2_WARNING(7323604, message);
            return Status(ErrorCodes::IllegalOperation, message);
        }

        auto const writer = ticketHolderManager->_writeTicketHolder.get();
        if (writer) {
            writer->setMaxQueueDepth(newWriteMaxQueueDepth);
            return Status::OK();
        }
        constexpr auto& message =
            "Attempting to update write max queue depth before the "
            "write TicketHolder is initialized";
        LOGV2_WARNING(6754203, message);
        return Status(ErrorCodes::IllegalOperation, message);
    }

    return Status::OK();
}

Status TicketHolderManager::updateReadMaxQueueDepth(std::int32_t newReadMaxQueueDepth) {
    if (auto const client = Client::getCurrent()) {
        auto const ticketHolderManager = TicketHolderManager::get(client->getServiceContext());
        if (!ticketHolderManager) {
            constexpr auto& message =
                "Attempting to modify read max queue depth on an instance without a "
                "storage engine";
            LOGV2_WARNING(7323605, message);
            return Status(ErrorCodes::IllegalOperation, message);
        }

        auto const reader = ticketHolderManager->_readTicketHolder.get();
        if (reader) {
            reader->setMaxQueueDepth(newReadMaxQueueDepth);
            return Status::OK();
        }
        constexpr auto& message =
            "Attempting to update read max queue depth before the "
            "write TicketHolder is initialized";
        LOGV2_WARNING(6754204, message);
        return Status(ErrorCodes::IllegalOperation, message);
    }

    return Status::OK();
}

Status TicketHolderManager::updateConcurrentWriteTransactions(const int32_t& newWriteTransactions) {
    if (auto client = Client::getCurrent()) {
        auto opCtx = client->getOperationContext();
        auto ticketHolderManager = TicketHolderManager::get(client->getServiceContext());
        if (!ticketHolderManager) {
            constexpr auto& message =
                "Attempting to modify write transactions limit on an instance without a "
                "storage engine";
            LOGV2_WARNING(7323602, message);
            return Status(ErrorCodes::IllegalOperation, message);
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
            writer->resize(opCtx, newWriteTransactions, Date_t::max());
            return Status::OK();
        }
        constexpr auto& message =
            "Attempting to update concurrent write transactions limit before the "
            "write TicketHolder is initialized";
        LOGV2_WARNING(6754202, message);
        return Status(ErrorCodes::IllegalOperation, message);
    }
    return Status::OK();
};

Status TicketHolderManager::updateConcurrentReadTransactions(const int32_t& newReadTransactions) {
    if (auto client = Client::getCurrent()) {
        auto opCtx = client->getOperationContext();
        auto ticketHolderManager = TicketHolderManager::get(client->getServiceContext());
        if (!ticketHolderManager) {
            constexpr auto& message =
                "Attempting to modify read transactions limit on an instance without a "
                "storage engine";
            LOGV2_WARNING(7323601, message);
            return Status(ErrorCodes::IllegalOperation, message);
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
            reader->resize(opCtx, newReadTransactions, Date_t::max());
            return Status::OK();
        }

        constexpr auto& message =
            "Attempting to update concurrent read transactions limit before the read "
            "TicketHolder is initialized";
        LOGV2_WARNING(6754201, message);
        return Status(ErrorCodes::IllegalOperation, message);
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
