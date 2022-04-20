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

#include "mongo/db/storage/ticketholders.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace {
const auto ticketHoldersDecoration =
    mongo::ServiceContext::declareDecoration<mongo::TicketHolders>();
}

namespace mongo {

Status TicketHolders::updateConcurrentWriteTransactions(const int& newWriteTransactions) {
    if (auto client = Client::getCurrent()) {
        if (auto svcCtx = client->getServiceContext()) {
            auto& ticketHolders = TicketHolders::get(svcCtx);
            auto& writer = ticketHolders._openWriteTransaction;
            if (writer) {
                return writer->resize(newWriteTransactions);
            }
        }
    }
    return Status::OK();
};

Status TicketHolders::updateConcurrentReadTransactions(const int& newReadTransactions) {
    if (auto client = Client::getCurrent()) {
        if (auto svcCtx = client->getServiceContext()) {
            auto& ticketHolders = TicketHolders::get(svcCtx);
            auto& reader = ticketHolders._openReadTransaction;
            if (reader) {
                return reader->resize(newReadTransactions);
            }
        }
    }
    return Status::OK();
}

void TicketHolders::setGlobalThrottling(std::unique_ptr<TicketHolder> reading,
                                        std::unique_ptr<TicketHolder> writing) {
    _openReadTransaction = std::move(reading);
    _openWriteTransaction = std::move(writing);
}

TicketHolder* TicketHolders::getTicketHolder(LockMode mode) {
    switch (mode) {
        case MODE_S:
        case MODE_IS:
            return _openReadTransaction.get();
        case MODE_IX:
            return _openWriteTransaction.get();
        default:
            return nullptr;
    }
}

TicketHolders& TicketHolders::get(ServiceContext* svcCtx) {
    return ticketHoldersDecoration(svcCtx);
}

TicketHolders& TicketHolders::get(ServiceContext& svcCtx) {
    return ticketHoldersDecoration(svcCtx);
}

}  // namespace mongo
