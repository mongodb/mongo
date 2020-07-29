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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_batcher_test_fixture.h"

#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
void OplogBufferMock::startup(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(!_hasShutDown);
    invariant(!_hasStartedUp);
    _hasStartedUp = true;
    clear(lk);
}

void OplogBufferMock::shutdown(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_hasStartedUp);
    clear(lk);
    _hasShutDown = true;
}

void OplogBufferMock::push(OperationContext* opCtx,
                           Batch::const_iterator begin,
                           Batch::const_iterator end) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    _data.insert(_data.end(), begin, end);
    _notEmptyCv.notify_one();
}

bool OplogBufferMock::isEmpty() const {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    return _curIndex == _data.size();
}

std::size_t OplogBufferMock::getSize() const {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    std::size_t total = 0;
    for (std::size_t i = _curIndex; i < _data.size(); i++) {
        total += _data[i].objsize();
    }
    return total;
}

std::size_t OplogBufferMock::getCount() const {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    return _data.size() - _curIndex;
}

void OplogBufferMock::clear(OperationContext* opCtx) {
    stdx::lock_guard<Latch> lk(_mutex);
    clear(lk);
}

void OplogBufferMock::clear(WithLock) {
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    _curIndex = 0;
    _data.clear();
}

bool OplogBufferMock::tryPop(OperationContext* opCtx, Value* value) {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    if (_curIndex == _data.size())
        return false;
    *value = _data[_curIndex++];
    return true;
}

bool OplogBufferMock::waitForData(Seconds waitDuration) {
    stdx::unique_lock<Latch> lk(_mutex);
    _notEmptyCv.wait_for(
        lk, waitDuration.toSystemDuration(), [&] { return _curIndex < _data.size(); });
    return _curIndex < _data.size();
}

bool OplogBufferMock::peek(OperationContext* opCtx, Value* value) {
    stdx::unique_lock<Latch> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    if (_curIndex == _data.size())
        return false;
    *value = _data[_curIndex];
    return true;
}

boost::optional<OplogBufferMock::Value> OplogBufferMock::lastObjectPushed(
    OperationContext* opCtx) const {
    stdx::unique_lock<Latch> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    if (_data.size() == _curIndex)
        return boost::none;
    return _data.back();
}

StatusWith<OplogBufferMock::Value> OplogBufferMock::findByTimestamp(OperationContext* opCtx,
                                                                    const Timestamp& ts) {
    stdx::unique_lock<Latch> lk(_mutex);
    for (const auto& item : _data) {
        if (item["ts"].timestamp() == ts) {
            return item;
        }
    }
    return {ErrorCodes::KeyNotFound,
            str::stream() << "No such timestamp in collection: " << ts.toString()};
}

Status OplogBufferMock::seekToTimestamp(OperationContext* opCtx, const Timestamp& ts, bool exact) {
    stdx::unique_lock<Latch> lk(_mutex);
    for (std::size_t i = 0; i < _data.size(); i++) {
        if (_data[i]["ts"].timestamp() == ts) {
            _curIndex = i;
            return Status::OK();
        } else if (_data[i]["ts"].timestamp() > ts) {
            _curIndex = i;
            break;
        }
    }
    if (!exact)
        return Status::OK();
    return {ErrorCodes::KeyNotFound, str::stream() << "Timestamp not found: " << ts.toString()};
}

/**
 * Generates an insert oplog entry with the given number used for the timestamp.
 */
OplogEntry makeInsertOplogEntry(int t, const NamespaceString& nss) {
    BSONObj oField = BSON("_id" << t << "a" << t);
    return OplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                      boost::none,                 // hash
                      OpTypeEnum::kInsert,         // op type
                      nss,                         // namespace
                      boost::none,                 // uuid
                      boost::none,                 // fromMigrate
                      OplogEntry::kOplogVersion,   // version
                      oField,                      // o
                      boost::none,                 // o2
                      {},                          // sessionInfo
                      boost::none,                 // upsert
                      Date_t() + Seconds(t),       // wall clock time
                      boost::none,                 // statement id
                      boost::none,   // optime of previous write within same transaction
                      boost::none,   // pre-image optime
                      boost::none,   // post-image optime
                      boost::none);  // ShardId of resharding recipient
}

/**
 * Generates an applyOps oplog entry with the given number used for the timestamp.
 */
OplogEntry makeApplyOpsOplogEntry(int t, bool prepare, const std::vector<OplogEntry>& innerOps) {
    auto nss = NamespaceString(NamespaceString::kAdminDb).getCommandNS();
    BSONObjBuilder oField;
    BSONArrayBuilder applyOpsBuilder = oField.subarrayStart("applyOps");
    for (const auto& op : innerOps) {
        applyOpsBuilder.append(op.getDurableReplOperation().toBSON());
    }
    applyOpsBuilder.doneFast();
    if (prepare) {
        oField.append("prepare", true);
    }
    return OplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                      boost::none,                 // hash
                      OpTypeEnum::kCommand,        // op type
                      nss,                         // namespace
                      boost::none,                 // uuid
                      boost::none,                 // fromMigrate
                      OplogEntry::kOplogVersion,   // version
                      oField.obj(),                // o
                      boost::none,                 // o2
                      {},                          // sessionInfo
                      boost::none,                 // upsert
                      Date_t() + Seconds(t),       // wall clock time
                      boost::none,                 // statement id
                      boost::none,   // optime of previous write within same transaction
                      boost::none,   // pre-image optime
                      boost::none,   // post-image optime
                      boost::none);  // ShardId of resharding recipient
}

/**
 * Generates a commitTransaction/applyOps oplog entry, depending on whether this is for a prepared
 * transaction, with the given number used for the timestamp.
 */
OplogEntry makeCommitTransactionOplogEntry(int t, StringData dbName, bool prepared, int count) {
    auto nss = NamespaceString(dbName).getCommandNS();
    BSONObj oField;
    if (prepared) {
        CommitTransactionOplogObject cmdObj;
        cmdObj.setCount(count);
        oField = cmdObj.toBSON();
    } else {
        oField = BSON("applyOps" << BSONArray() << "count" << count);
    }
    return OplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                      boost::none,                 // hash
                      OpTypeEnum::kCommand,        // op type
                      nss,                         // namespace
                      boost::none,                 // uuid
                      boost::none,                 // fromMigrate
                      OplogEntry::kOplogVersion,   // version
                      oField,                      // o
                      boost::none,                 // o2
                      {},                          // sessionInfo
                      boost::none,                 // upsert
                      Date_t() + Seconds(t),       // wall clock time
                      boost::none,                 // statement id
                      boost::none,   // optime of previous write within same transaction
                      boost::none,   // pre-image optime
                      boost::none,   // post-image optime
                      boost::none);  // ShardId of resharding recipient
}

/**
 * Creates oplog entries that are meant to be all parts of a mocked large transaction. This function
 * does the following:
 *
 * 1. If we intend to make the first oplog entry of the transaction, we add a Null prevOptime to
 *    denote that there is no entry that comes before this one. This entry will just be a applyOps.
 * 2. If we intend to make the last oplog entry of the transaction, then we make a commit oplog
 *    entry.
 * 3. Otherwise, we create applyOps oplog entries that denote all of the intermediate oplog entries.
 */
OplogEntry makeLargeTransactionOplogEntries(int t,
                                            bool prepared,
                                            bool isFirst,
                                            bool isLast,
                                            int curr,
                                            int count,
                                            const std::vector<OplogEntry> innerOps) {
    auto nss = NamespaceString(NamespaceString::kAdminDb).getCommandNS();
    OpTime prevWriteOpTime = isFirst ? OpTime() : OpTime(Timestamp(t - 1, 1), 1);
    BSONObj oField;
    if (isLast && prepared) {
        // Makes a commit command oplog entry if this is the last oplog entry we wish to create.
        CommitTransactionOplogObject cmdObj;
        cmdObj.setCount(count);
        oField = cmdObj.toBSON();
        invariant(innerOps.empty());
    } else {
        BSONObjBuilder oFieldBuilder;
        BSONArrayBuilder applyOpsBuilder = oFieldBuilder.subarrayStart("applyOps");
        for (const auto& op : innerOps) {
            applyOpsBuilder.append(op.getDurableReplOperation().toBSON());
        }
        applyOpsBuilder.doneFast();
        if (!isLast) {
            if (prepared && curr == count - 1) {
                oFieldBuilder.append("prepare", true);
            }
            oFieldBuilder.append("partialTxn", true);
        }
        oField = oFieldBuilder.obj();
    }
    return OplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                      boost::none,                 // hash
                      OpTypeEnum::kCommand,        // op type
                      nss,                         // namespace
                      boost::none,                 // uuid
                      boost::none,                 // fromMigrate
                      OplogEntry::kOplogVersion,   // version
                      oField,                      // o
                      boost::none,                 // o2
                      {},                          // sessionInfo
                      boost::none,                 // upsert
                      Date_t() + Seconds(t),       // wall clock time
                      boost::none,                 // statement id
                      prevWriteOpTime,  // optime of previous write within same transaction
                      boost::none,      // pre-image optime
                      boost::none,      // post-image optime
                      boost::none);     // ShardId of resharding recipient
}

/**
 * Generates a mock large-transaction which has more than one oplog entry.
 */
std::vector<OplogEntry> makeMultiEntryTransactionOplogEntries(int t,
                                                              StringData dbName,
                                                              bool prepared,
                                                              int count) {
    ASSERT_GTE(count, 2);
    std::vector<OplogEntry> vec;
    for (int i = 0; i < count; i++) {
        vec.push_back(makeLargeTransactionOplogEntries(
            t + i, prepared, i == 0, i == count - 1, i + 1, count, {}));
    }
    return vec;
}

/**
 * Generates a mock large-transaction which has more than one oplog entry and contains the
 * operations in innerOps.
 */
std::vector<OplogEntry> makeMultiEntryTransactionOplogEntries(
    int t, StringData dbName, bool prepared, std::vector<std::vector<OplogEntry>> innerOps) {
    std::size_t count = innerOps.size() + (prepared ? 1 : 0);
    ASSERT_GTE(count, 2);
    std::vector<OplogEntry> vec;
    for (std::size_t i = 0; i < count; i++) {
        vec.push_back(makeLargeTransactionOplogEntries(
            t + i,
            prepared,
            i == 0,
            i == count - 1,
            i + 1,
            count,
            i < innerOps.size() ? innerOps[i] : std::vector<OplogEntry>()));
    }
    return vec;
}

/**
 * Returns string representation of std::vector<OplogEntry>.
 */
std::string toString(const std::vector<OplogEntry>& ops) {
    StringBuilder sb;
    sb << "[";
    for (const auto& op : ops) {
        sb << " " << op.toString();
    }
    sb << " ]";
    return sb.str();
}

}  // namespace repl
}  // namespace mongo
