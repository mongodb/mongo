/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/admission/ticketing_system.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/decorable.h"

#include <utility>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace admission {

namespace {
const auto ticketingSystemDecoration =
    mongo::ServiceContext::declareDecoration<std::unique_ptr<TicketingSystem>>();

template <typename Updater>
Status updateSettings(const std::string& op, Updater&& updater) {
    if (auto client = Client::getCurrent()) {
        auto ticketingSystem = TicketingSystem::get(client->getServiceContext());
        if (!ticketingSystem) {
            auto message =
                fmt::format("Attempting to modify {} on an instance without a storage engine", op);
            return {ErrorCodes::IllegalOperation, message};
        }

        return updater(client, ticketingSystem);
    }

    return Status::OK();
}
}  // namespace

Status TicketingSystem::NormalPrioritySettings::updateWriteMaxQueueDepth(
    std::int32_t newWriteMaxQueueDepth) {
    return updateSettings("write max queue depth", [=](Client*, TicketingSystem* ticketingSystem) {
        ticketingSystem->setMaxQueueDepth(
            AdmissionContext::Priority::kNormal, Operation::kWrite, newWriteMaxQueueDepth);
        return Status::OK();
    });
}

Status TicketingSystem::NormalPrioritySettings::updateReadMaxQueueDepth(
    std::int32_t newReadMaxQueueDepth) {
    return updateSettings("read max queue depth", [=](Client*, TicketingSystem* ticketingSystem) {
        ticketingSystem->setMaxQueueDepth(
            AdmissionContext::Priority::kNormal, Operation::kRead, newReadMaxQueueDepth);
        return Status::OK();
    });
}

Status TicketingSystem::NormalPrioritySettings::updateConcurrentWriteTransactions(
    const int32_t& newWriteTransactions) {
    return updateSettings(
        "write transactions limit", [&](Client* client, TicketingSystem* ticketingSystem) {
            if (!ticketingSystem->isRuntimeResizable()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent write transactions limit when it is being "
                              "dynamically adjusted"};
            }
            ticketingSystem->setConcurrentTransactions(client->getOperationContext(),
                                                       AdmissionContext::Priority::kNormal,
                                                       Operation::kWrite,
                                                       newWriteTransactions);
            return Status::OK();
        });
}

Status TicketingSystem::NormalPrioritySettings::updateConcurrentReadTransactions(
    const int32_t& newReadTransactions) {
    return updateSettings(
        "read transactions limit", [&](Client* client, TicketingSystem* ticketingSystem) {
            if (!ticketingSystem->isRuntimeResizable()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent read transactions limit when it is being "
                              "dynamically adjusted"};
            }
            ticketingSystem->setConcurrentTransactions(client->getOperationContext(),
                                                       AdmissionContext::Priority::kNormal,
                                                       Operation::kRead,
                                                       newReadTransactions);
            return Status::OK();
        });
}

Status TicketingSystem::NormalPrioritySettings::validateConcurrentWriteTransactions(
    const int32_t& newWriteTransactions, const boost::optional<TenantId>) {
    if (!getTestCommandsEnabled() && newWriteTransactions < 5) {
        return Status(ErrorCodes::BadValue,
                      "Concurrent write transactions limit must be greater than or equal to 5.");
    }
    return Status::OK();
}

Status TicketingSystem::NormalPrioritySettings::validateConcurrentReadTransactions(
    const int32_t& newReadTransactions, const boost::optional<TenantId>) {
    if (!getTestCommandsEnabled() && newReadTransactions < 5) {
        return Status(ErrorCodes::BadValue,
                      "Concurrent read transactions limit must be greater than or equal to 5.");
    }
    return Status::OK();
}

Status TicketingSystem::LowPrioritySettings::updateWriteMaxQueueDepth(
    std::int32_t newWriteMaxQueueDepth) {
    return updateSettings("write max queue depth", [=](Client*, TicketingSystem* ticketingSystem) {
        if (!ticketingSystem->usesPrioritization()) {
            return Status{ErrorCodes::IllegalOperation,
                          "Cannot modify concurrent low priority settings if the storage "
                          "engine concurrency adjustment algorithm is using a single pool "
                          "without prioritization"};
        }
        ticketingSystem->setMaxQueueDepth(
            AdmissionContext::Priority::kLow, Operation::kWrite, newWriteMaxQueueDepth);
        return Status::OK();
    });
}

Status TicketingSystem::LowPrioritySettings::updateReadMaxQueueDepth(
    std::int32_t newReadMaxQueueDepth) {
    return updateSettings("read max queue depth", [=](Client*, TicketingSystem* ticketingSystem) {
        if (!ticketingSystem->usesPrioritization()) {
            return Status{ErrorCodes::IllegalOperation,
                          "Cannot modify concurrent low priority settings if the storage "
                          "engine concurrency adjustment algorithm is using a single pool "
                          "without prioritization"};
        }
        ticketingSystem->setMaxQueueDepth(
            AdmissionContext::Priority::kLow, Operation::kRead, newReadMaxQueueDepth);
        return Status::OK();
    });
}

Status TicketingSystem::LowPrioritySettings::updateConcurrentWriteTransactions(
    const int32_t& newWriteTransactions) {
    return updateSettings(
        "write transactions limit", [&](Client* client, TicketingSystem* ticketingSystem) {
            if (!ticketingSystem->isRuntimeResizable()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent write transactions limit when it is being "
                              "dynamically adjusted"};
            }
            if (!ticketingSystem->usesPrioritization()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent low priority settings if the storage "
                              "engine concurrency adjustment algorithm is using a single pool "
                              "without prioritization"};
            }
            ticketingSystem->setConcurrentTransactions(client->getOperationContext(),
                                                       AdmissionContext::Priority::kLow,
                                                       Operation::kWrite,
                                                       newWriteTransactions);
            return Status::OK();
        });
}

Status TicketingSystem::LowPrioritySettings::updateConcurrentReadTransactions(
    const int32_t& newReadTransactions) {
    return updateSettings(
        "read transactions limit", [&](Client* client, TicketingSystem* ticketingSystem) {
            if (!ticketingSystem->isRuntimeResizable()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent read transactions limit when it is being "
                              "dynamically adjusted"};
            }
            if (!ticketingSystem->usesPrioritization()) {
                return Status{ErrorCodes::IllegalOperation,
                              "Cannot modify concurrent low priority settings if the storage "
                              "engine concurrency adjustment algorithm is using a single pool "
                              "without prioritization"};
            }
            ticketingSystem->setConcurrentTransactions(client->getOperationContext(),
                                                       AdmissionContext::Priority::kLow,
                                                       Operation::kRead,
                                                       newReadTransactions);
            return Status::OK();
        });
}

TicketingSystem* TicketingSystem::get(ServiceContext* svcCtx) {
    return ticketingSystemDecoration(svcCtx).get();
}

void TicketingSystem::use(ServiceContext* svcCtx,
                          std::unique_ptr<TicketingSystem> newTicketingSystem) {
    ticketingSystemDecoration(svcCtx) = std::move(newTicketingSystem);
}

}  // namespace admission
}  // namespace mongo
