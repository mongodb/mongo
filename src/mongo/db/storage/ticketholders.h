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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/service_context.h"

#include <memory>

namespace mongo {

class TicketHolder;

class TicketHolders {
public:
    static Status updateConcurrentWriteTransactions(const int& newWriteTransactions);

    static Status updateConcurrentReadTransactions(const int& newReadTransactions);

    static TicketHolders& get(ServiceContext* svcCtx);
    static TicketHolders& get(ServiceContext& svcCtx);
    /**
     * Sets the TicketHolder implementation to use to obtain tickets from 'reading' (for MODE_S and
     * MODE_IS), and from 'writing' (for MODE_IX) in order to throttle database access. There is no
     * throttling for MODE_X, as there can only ever be a single locker using this mode. The
     * throttling is intended to defend against large drops in throughput under high load due to too
     * much concurrency.
     */
    void setGlobalThrottling(std::unique_ptr<TicketHolder> reading,
                             std::unique_ptr<TicketHolder> writing);

    TicketHolder* getTicketHolder(LockMode mode);

private:
    std::unique_ptr<TicketHolder> _openWriteTransaction;
    std::unique_ptr<TicketHolder> _openReadTransaction;
};

}  // namespace mongo
