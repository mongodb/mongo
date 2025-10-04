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

#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/operation_logger.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <vector>

namespace MONGO_MOD_PUB mongo {

class OperationLoggerImpl : public OperationLogger {
    OperationLoggerImpl(const OperationLoggerImpl&) = delete;
    OperationLoggerImpl& operator=(const OperationLoggerImpl&) = delete;

public:
    OperationLoggerImpl() = default;
    ~OperationLoggerImpl() override = default;

    void appendOplogEntryChainInfo(OperationContext* opCtx,
                                   repl::MutableOplogEntry* oplogEntry,
                                   repl::OplogLink* oplogLink,
                                   const std::vector<StmtId>& stmtIds) override;

    repl::OpTime logOp(OperationContext* opCtx, repl::MutableOplogEntry* oplogEntry) override;

    void logOplogRecords(OperationContext* opCtx,
                         const NamespaceString& nss,
                         std::vector<Record>* records,
                         const std::vector<Timestamp>& timestamps,
                         const CollectionPtr& oplogCollection,
                         repl::OpTime finalOpTime,
                         Date_t wallTime) override;

    std::vector<OplogSlot> getNextOpTimes(OperationContext* opCtx, std::size_t count) override;
};

}  // namespace MONGO_MOD_PUB mongo
