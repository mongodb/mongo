/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {
// This is a simple random-access "oplog buffer" based on a vector.
class OplogBufferMock : public RandomAccessOplogBuffer {
public:
    OplogBufferMock() : RandomAccessOplogBuffer() {};

    void startup(OperationContext* opCtx) final;
    void shutdown(OperationContext* opCtx) final;
    void push(OperationContext* opCtx,
              Batch::const_iterator begin,
              Batch::const_iterator end,
              boost::optional<const Cost&> cost = boost::none) final;
    void waitForSpace(OperationContext* opCtx, const Cost& cost) final {};
    bool isEmpty() const final;
    std::size_t getSize() const final;
    std::size_t getCount() const final;
    void clear(OperationContext* opCtx) final;
    bool tryPop(OperationContext* opCtx, Value* value) final;
    bool waitForDataFor(Milliseconds waitDuration, Interruptible* interruptible) final;
    bool waitForDataUntil(Date_t deadline, Interruptible* interruptible) final;
    bool peek(OperationContext* opCtx, Value* value) final;
    boost::optional<Value> lastObjectPushed(OperationContext* opCtx) const final;
    StatusWith<Value> findByTimestamp(OperationContext* opCtx, const Timestamp& ts) final;
    Status seekToTimestamp(OperationContext* opCtx,
                           const Timestamp& ts,
                           SeekStrategy exact = SeekStrategy::kExact) final;

private:
    mutable stdx::mutex _mutex;
    stdx::condition_variable _notEmptyCv;
    bool _hasShutDown = false;
    bool _hasStartedUp = false;
    std::vector<Value> _data;
    std::size_t _curIndex = 0;

    void clear(WithLock);
};


// Convenience routines for creating oplog entries to test with.
OplogEntry makeInsertOplogEntry(int t,
                                const NamespaceString& nss,
                                boost::optional<UUID> uuid = boost::none,
                                std::int64_t version = repl::OplogEntry::kOplogVersion);

OplogEntry makeDBCheckBatchEntry(int t,
                                 const NamespaceString& nss,
                                 boost::optional<UUID> uuid = boost::none);

OplogEntry makeNewPrimaryBatchEntry(int t);

OplogEntry makeApplyOpsOplogEntry(int t,
                                  bool prepare,
                                  const std::vector<OplogEntry>& innerOps = {});

OplogEntry makeCommitTransactionOplogEntry(int t,
                                           const DatabaseName& dbName,
                                           bool prepared,
                                           boost::optional<int> count = boost::none);

OplogEntry makeAbortTransactionOplogEntry(int t, const DatabaseName& dbName);

std::vector<OplogEntry> makeMultiEntryTransactionOplogEntries(int t,
                                                              const DatabaseName& dbName,
                                                              bool prepared,
                                                              int count);

OplogEntry makeTruncateRangeEntry(int t, const NamespaceString& nss, const RecordId& maxRecordId);

OplogEntry makeTruncateRangeOnPreImagesEntry(int t, int maxTruncateTimestamp);

OplogEntry makeTruncateRangeOnOplogEntry(int t, int maxTruncateTimestamp);

std::string toString(const std::vector<OplogEntry>& ops);
}  // namespace repl
}  // namespace mongo
