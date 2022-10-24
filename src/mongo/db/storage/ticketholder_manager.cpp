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
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/ticketholder.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace {
const auto ticketHolderManagerDecoration =
    mongo::ServiceContext::declareDecoration<std::unique_ptr<mongo::TicketHolderManager>>();
}

namespace mongo {

Status TicketHolderManager::updateConcurrentWriteTransactions(const int& newWriteTransactions) {
    if (auto client = Client::getCurrent()) {
        if (auto svcCtx = client->getServiceContext()) {
            auto ticketHolderManager = TicketHolderManager::get(svcCtx);
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
    }
    return Status::OK();
};

Status TicketHolderManager::updateConcurrentReadTransactions(const int& newReadTransactions) {
    if (auto client = Client::getCurrent()) {
        if (auto svcCtx = client->getServiceContext()) {
            auto ticketHolderManager = TicketHolderManager::get(svcCtx);
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
}
}  // namespace mongo
