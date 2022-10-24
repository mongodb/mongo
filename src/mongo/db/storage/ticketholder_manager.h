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
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {

class TicketHolder;

/**
 * A ticket mechanism is required for global lock acquisition to reduce contention on storage engine
 * resources.
 *
 * Each TicketHolder maintains a pool of n available tickets. The TicketHolderManager is responsible
 * for sizing each ticket pool and determining which ticket pool a caller should use for ticket
 * acquisition.
 *
 */
class TicketHolderManager {
public:
    TicketHolderManager(std::unique_ptr<TicketHolder> readTicketHolder,
                        std::unique_ptr<TicketHolder> writeTicketHolder)
        : _readTicketHolder(std::move(readTicketHolder)),
          _writeTicketHolder(std::move(writeTicketHolder)){};

    ~TicketHolderManager(){};

    static Status updateConcurrentWriteTransactions(const int& newWriteTransactions);
    static Status updateConcurrentReadTransactions(const int& newReadTransactions);

    static TicketHolderManager* get(ServiceContext* svcCtx);

    static void use(ServiceContext* svcCtx,
                    std::unique_ptr<TicketHolderManager> newTicketHolderManager);

    /**
     * Given the 'mode' a user requests for a GlobalLock, returns the corresponding TicketHolder.
     */
    TicketHolder* getTicketHolder(LockMode mode);

    void appendStats(BSONObjBuilder& b);

private:
    /**
     * Holds tickets for MODE_S/MODE_IS global locks requests.
     */
    std::unique_ptr<TicketHolder> _readTicketHolder;

    /**
     * Holds tickets for MODE_X/MODE_IX global lock requests.
     */
    std::unique_ptr<TicketHolder> _writeTicketHolder;
};
}  // namespace mongo
