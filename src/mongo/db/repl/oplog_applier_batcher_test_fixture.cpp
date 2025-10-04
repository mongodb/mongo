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

#include "mongo/db/repl/oplog_applier_batcher_test_fixture.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/truncate_range_oplog_entry_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {
void OplogBufferMock::startup(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(!_hasShutDown);
    invariant(!_hasStartedUp);
    _hasStartedUp = true;
    clear(lk);
}

void OplogBufferMock::shutdown(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_hasStartedUp);
    clear(lk);
    _hasShutDown = true;
    _notEmptyCv.notify_all();
}

void OplogBufferMock::push(OperationContext* opCtx,
                           Batch::const_iterator begin,
                           Batch::const_iterator end,
                           boost::optional<const Cost&> cost) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    _data.insert(_data.end(), begin, end);
    _notEmptyCv.notify_one();
}

bool OplogBufferMock::isEmpty() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    return _curIndex == _data.size();
}

std::size_t OplogBufferMock::getSize() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    std::size_t total = 0;
    for (std::size_t i = _curIndex; i < _data.size(); i++) {
        total += _data[i].objsize();
    }
    return total;
}

std::size_t OplogBufferMock::getCount() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    return _data.size() - _curIndex;
}

void OplogBufferMock::clear(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    clear(lk);
}

void OplogBufferMock::clear(WithLock) {
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    _curIndex = 0;
    _data.clear();
}

bool OplogBufferMock::tryPop(OperationContext* opCtx, Value* value) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    if (_curIndex == _data.size())
        return false;
    *value = _data[_curIndex++];
    return true;
}

bool OplogBufferMock::waitForDataFor(Milliseconds waitDuration, Interruptible* interruptible) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    interruptible->waitForConditionOrInterruptFor(
        _notEmptyCv, lk, waitDuration, [&] { return _hasShutDown || _curIndex < _data.size(); });
    return _curIndex < _data.size();
}

bool OplogBufferMock::waitForDataUntil(Date_t deadline, Interruptible* interruptible) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    interruptible->waitForConditionOrInterruptUntil(
        _notEmptyCv, lk, deadline, [&] { return _hasShutDown || _curIndex < _data.size(); });
    return _curIndex < _data.size();
}

bool OplogBufferMock::peek(OperationContext* opCtx, Value* value) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    if (_curIndex == _data.size())
        return false;
    *value = _data[_curIndex];
    return true;
}

boost::optional<OplogBufferMock::Value> OplogBufferMock::lastObjectPushed(
    OperationContext* opCtx) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_hasStartedUp);
    invariant(!_hasShutDown);
    if (_data.size() == _curIndex)
        return boost::none;
    return _data.back();
}

StatusWith<OplogBufferMock::Value> OplogBufferMock::findByTimestamp(OperationContext* opCtx,
                                                                    const Timestamp& ts) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    for (const auto& item : _data) {
        if (item["ts"].timestamp() == ts) {
            return item;
        }
    }
    return {ErrorCodes::KeyNotFound,
            str::stream() << "No such timestamp in collection: " << ts.toString()};
}

Status OplogBufferMock::seekToTimestamp(OperationContext* opCtx,
                                        const Timestamp& ts,
                                        SeekStrategy exact) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    for (std::size_t i = 0; i < _data.size(); i++) {
        if (_data[i]["ts"].timestamp() == ts) {
            _curIndex = i;
            return Status::OK();
        } else if (_data[i]["ts"].timestamp() > ts) {
            _curIndex = i;
            break;
        }
    }
    if (exact != SeekStrategy::kExact)
        return Status::OK();
    return {ErrorCodes::KeyNotFound, str::stream() << "Timestamp not found: " << ts.toString()};
}

/**
 * Generates an insert oplog entry with the given number used for the timestamp.
 */
OplogEntry makeInsertOplogEntry(int t,
                                const NamespaceString& nss,
                                boost::optional<UUID> uuid,
                                std::int64_t version) {
    BSONObj oField = BSON("_id" << t << "a" << t);
    return {DurableOplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                              OpTypeEnum::kInsert,         // op type
                              nss,                         // namespace
                              uuid,                        // uuid
                              boost::none,                 // fromMigrate
                              boost::none,                 // checkExistenceForDiffInsert
                              boost::none,                 // versionContext
                              version,                     // version
                              oField,                      // o
                              boost::none,                 // o2
                              {},                          // sessionInfo
                              boost::none,                 // upsert
                              Date_t() + Seconds(t),       // wall clock time
                              {},                          // statement ids
                              boost::none,    // optime of previous write within same transaction
                              boost::none,    // pre-image optime
                              boost::none,    // post-image optime
                              boost::none,    // ShardId of resharding recipient
                              boost::none,    // _id
                              boost::none)};  // needsRetryImage
}

/**
 * Generates a dbcheck batch oplog entry with the given number used for the timestamp.
 */
OplogEntry makeDBCheckBatchEntry(int t, const NamespaceString& nss, boost::optional<UUID> uuid) {
    BSONObj oField =
        BSON("dbCheck" << nss.ns_forTest() << "type"
                       << "batch"
                       << "md5"
                       << "9e6d9948e85826fa992fd5c819d02dd6"
                       << "minKey" << 1 << "maxKey" << 3 << "readTimestamp" << Timestamp(t, 1));
    return {DurableOplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                              OpTypeEnum::kCommand,        // op type
                              nss,                         // namespace
                              uuid,                        // uuid
                              boost::none,                 // fromMigrate
                              boost::none,                 // checkExistenceForDiffInsert
                              boost::none,                 // versionContext
                              OplogEntry::kOplogVersion,   // version
                              oField,                      // o
                              boost::none,                 // o2
                              {},                          // sessionInfo
                              boost::none,                 // upsert
                              Date_t() + Seconds(t),       // wall clock time
                              {},                          // statement ids
                              boost::none,    // optime of previous write within same transaction
                              boost::none,    // pre-image optime
                              boost::none,    // post-image optime
                              boost::none,    // ShardId of resharding recipient
                              boost::none,    // _id
                              boost::none)};  // needsRetryImage
}

/**
 * Generates a New Primary batch oplog entry with the given number used for the timestamp.
 */
OplogEntry makeNewPrimaryBatchEntry(int t) {
    BSONObj oField = BSON(kNewPrimaryMsgField << kNewPrimaryMsg);
    return {DurableOplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                              OpTypeEnum::kNoop,           // op type
                              {},                          // namespace
                              boost::none,                 // uuid
                              boost::none,                 // fromMigrate
                              boost::none,                 // checkExistenceForDiffInsert
                              boost::none,                 // versionContext
                              OplogEntry::kOplogVersion,   // version
                              oField,                      // o
                              boost::none,                 // o2
                              {},                          // sessionInfo
                              boost::none,                 // upsert
                              Date_t() + Seconds(t),       // wall clock time
                              {},                          // statement ids
                              boost::none,    // optime of previous write within same transaction
                              boost::none,    // pre-image optime
                              boost::none,    // post-image optime
                              boost::none,    // ShardId of resharding recipient
                              boost::none,    // _id
                              boost::none)};  // needsRetryImage
}

/**
 * Generates an applyOps oplog entry with the given number used for the timestamp.
 */
OplogEntry makeApplyOpsOplogEntry(int t, bool prepare, const std::vector<OplogEntry>& innerOps) {
    auto nss = NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin).getCommandNS();
    BSONObjBuilder oField;
    BSONArrayBuilder applyOpsBuilder = oField.subarrayStart("applyOps");
    for (const auto& op : innerOps) {
        applyOpsBuilder.append(op.getDurableReplOperation().toBSON());
    }
    applyOpsBuilder.doneFast();
    if (prepare) {
        oField.append("prepare", true);
    }
    return {DurableOplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                              OpTypeEnum::kCommand,        // op type
                              nss,                         // namespace
                              boost::none,                 // uuid
                              boost::none,                 // fromMigrate
                              boost::none,                 // checkExistenceForDiffInsert
                              boost::none,                 // versionContext
                              OplogEntry::kOplogVersion,   // version
                              oField.obj(),                // o
                              boost::none,                 // o2
                              {},                          // sessionInfo
                              boost::none,                 // upsert
                              Date_t() + Seconds(t),       // wall clock time
                              {},                          // statement ids
                              boost::none,    // optime of previous write within same transaction
                              boost::none,    // pre-image optime
                              boost::none,    // post-image optime
                              boost::none,    // ShardId of resharding recipient
                              boost::none,    // _id
                              boost::none)};  // needsRetryImage
}

/**
 * Generates a commitTransaction/applyOps oplog entry, depending on whether this is for a prepared
 * transaction, with the given number used for the timestamp.
 */
OplogEntry makeCommitTransactionOplogEntry(int t,
                                           const DatabaseName& dbName,
                                           bool prepared,
                                           boost::optional<int> count) {
    auto nss = NamespaceString::createNamespaceString_forTest(dbName).getCommandNS();
    BSONObj oField;
    boost::optional<OpTime> prevWriteOpTime;

    if (prepared) {
        invariant(!count);
        CommitTransactionOplogObject cmdObj;
        oField = cmdObj.toBSON();
        prevWriteOpTime.emplace(OpTime(Timestamp(t, 0), 1));
    } else {
        invariant(count);
        BSONArrayBuilder applyOpsBuilder;
        for (int i = 0; i < *count; i++) {
            applyOpsBuilder.append(BSONObj());
        }
        oField = BSON("applyOps" << applyOpsBuilder.done());
    }

    return {DurableOplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                              OpTypeEnum::kCommand,        // op type
                              nss,                         // namespace
                              boost::none,                 // uuid
                              boost::none,                 // fromMigrate
                              boost::none,                 // checkExistenceForDiffInsert
                              boost::none,                 // versionContext
                              OplogEntry::kOplogVersion,   // version
                              oField,                      // o
                              boost::none,                 // o2
                              {},                          // sessionInfo
                              boost::none,                 // upsert
                              Date_t() + Seconds(t),       // wall clock time
                              {},                          // statement ids
                              prevWriteOpTime,             // prevWriteOpTimeInTransaction
                              boost::none,                 // pre-image optime
                              boost::none,                 // post-image optime
                              boost::none,                 // ShardId of resharding recipient
                              boost::none,                 // _id
                              boost::none)};               // needsRetryImage
}

/**
 * Generates an abortTransaction oplog entry with the given number used for the timestamp.
 */
OplogEntry makeAbortTransactionOplogEntry(int t, const DatabaseName& dbName) {
    auto nss = NamespaceString::createNamespaceString_forTest(dbName).getCommandNS();
    BSONObj oField;

    AbortTransactionOplogObject cmdObj;
    oField = cmdObj.toBSON();

    return {DurableOplogEntry(OpTime(Timestamp(t, 2), 1),  // optime
                              OpTypeEnum::kCommand,        // op type
                              nss,                         // namespace
                              boost::none,                 // uuid
                              boost::none,                 // fromMigrate
                              boost::none,                 // checkExistenceForDiffInsert
                              boost::none,                 // versionContext
                              OplogEntry::kOplogVersion,   // version
                              oField,                      // o
                              boost::none,                 // o2
                              {},                          // sessionInfo
                              boost::none,                 // upsert
                              Date_t() + Seconds(t),       // wall clock time
                              {},                          // statement ids
                              OpTime(Timestamp(t, 1), 1),  // prevWriteOpTimeInTransaction
                              boost::none,                 // pre-image optime
                              boost::none,                 // post-image optime
                              boost::none,                 // ShardId of resharding recipient
                              boost::none,                 // _id
                              boost::none)};               // needsRetryImage
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
    auto nss = NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin).getCommandNS();
    OpTime prevWriteOpTime = isFirst ? OpTime() : OpTime(Timestamp(t - 1, 1), 1);
    BSONObj oField;
    if (isLast && prepared) {
        // Makes a commit command oplog entry if this is the last oplog entry we wish to create.
        CommitTransactionOplogObject cmdObj;
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
    return {DurableOplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                              OpTypeEnum::kCommand,        // op type
                              nss,                         // namespace
                              boost::none,                 // uuid
                              boost::none,                 // fromMigrate
                              boost::none,                 // checkExistenceForDiffInsert
                              boost::none,                 // versionContext
                              OplogEntry::kOplogVersion,   // version
                              oField,                      // o
                              boost::none,                 // o2
                              {},                          // sessionInfo
                              boost::none,                 // upsert
                              Date_t() + Seconds(t),       // wall clock time
                              {},                          // statement ids
                              prevWriteOpTime,  // optime of previous write within same transaction
                              boost::none,      // pre-image optime
                              boost::none,      // post-image optime
                              boost::none,      // ShardId of resharding recipient
                              boost::none,      // _id
                              boost::none)};    // needsRetryImage
}

/**
 * Creates oplog entries that are meant to be all parts of a batched retryable write. This function
 * does the following:
 *
 * 1. We add multiOpType: 1 to all entries.
 * 2. We add a statement ID to all entries.
 * 3. If we intend to make the first oplog entry of the write, we add a Null prevOptime to
 *    denote that there is no entry that comes before this one.
 */
OplogEntry makeLargeRetryableWriteOplogEntries(int t,
                                               bool isFirst,
                                               const OperationSessionInfo& sessionInfo,
                                               StmtId startingStmtId,
                                               const std::vector<OplogEntry> innerOps) {
    auto nss = NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin).getCommandNS();
    OpTime prevWriteOpTime = isFirst ? OpTime() : OpTime(Timestamp(t - 1, 1), 1);
    BSONObj oField;
    BSONObjBuilder oFieldBuilder;
    BSONArrayBuilder applyOpsBuilder = oFieldBuilder.subarrayStart("applyOps");
    for (const auto& op : innerOps) {
        applyOpsBuilder.append(op.getDurableReplOperation().toBSON().addField(
            BSON("stmtId" << startingStmtId).firstElement()));
        startingStmtId++;
    }
    applyOpsBuilder.doneFast();
    oField = oFieldBuilder.obj();
    auto durableEntry =
        DurableOplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                          OpTypeEnum::kCommand,        // op type
                          nss,                         // namespace
                          boost::none,                 // uuid
                          boost::none,                 // fromMigrate
                          boost::none,                 // checkExistenceForDiffInsert
                          boost::none,                 // versionContext
                          OplogEntry::kOplogVersion,   // version
                          oField,                      // o
                          boost::none,                 // o2
                          sessionInfo,                 // sessionInfo
                          boost::none,                 // upsert
                          Date_t() + Seconds(t),       // wall clock time
                          {},                          // statement ids
                          prevWriteOpTime,  // optime of previous write within same transaction
                          boost::none,      // pre-image optime
                          boost::none,      // post-image optime
                          boost::none,      // ShardId of resharding recipient
                          boost::none,      // _id
                          boost::none);     // needsRetryImage
    return unittest::assertGet(OplogEntry::parse(durableEntry.toBSON().addField(
        BSON(OplogEntry::kMultiOpTypeFieldName << MultiOplogEntryType::kApplyOpsAppliedSeparately)
            .firstElement())));
}

/**
 * Generates a truncateRange oplog entry
 */
OplogEntry makeTruncateRangeEntry(int t, const NamespaceString& nss, const RecordId& maxRecordId) {
    TruncateRangeOplogEntry oField(std::string(nss.coll()), RecordId(), maxRecordId, 0, 0);
    return {DurableOplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                              OpTypeEnum::kCommand,        // op type
                              nss.getCommandNS(),          // namespace
                              boost::none,                 // uuid
                              boost::none,                 // fromMigrate
                              boost::none,                 // checkExistenceForDiffInsert
                              boost::none,                 // versionContext
                              OplogEntry::kOplogVersion,   // version
                              oField.toBSON(),             // o
                              boost::none,                 // o2
                              {},                          // sessionInfo
                              boost::none,                 // upsert
                              Date_t() + Seconds(t),       // wall clock time
                              {},                          // statement ids
                              boost::none,    // optime of previous write within same transaction
                              boost::none,    // pre-image optime
                              boost::none,    // post-image optime
                              boost::none,    // ShardId of resharding recipient
                              boost::none,    // _id
                              boost::none)};  // needsRetryImage
}

/**
 * Generates a truncateRange oplog entry that truncates the pre-images collection
 */
OplogEntry makeTruncateRangeOnPreImagesEntry(int t, int maxTruncateTimestamp) {
    ChangeStreamPreImageId preImageId(UUID::gen(), Timestamp(maxTruncateTimestamp, 1), 0);
    return makeTruncateRangeEntry(t,
                                  NamespaceString::kChangeStreamPreImagesNamespace,
                                  change_stream_pre_image_util::toRecordId(preImageId));
}

/**
 * Generates a truncateRange oplog entry that truncates the oplog collection
 */
OplogEntry makeTruncateRangeOnOplogEntry(int t, int maxTruncateTimestamp) {
    return makeTruncateRangeEntry(
        t,
        NamespaceString::kRsOplogNamespace,
        record_id_helpers::keyForOptime(Timestamp(maxTruncateTimestamp, 1), KeyFormat::String)
            .getValue());
}

/**
 * Generates a mock large-transaction which has more than one oplog entry.
 */
std::vector<OplogEntry> makeMultiEntryTransactionOplogEntries(int t,
                                                              const DatabaseName& dbName,
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
 * Returns string representation of std::vector<OplogEntry>.
 */
std::string toString(const std::vector<OplogEntry>& ops) {
    StringBuilder sb;
    sb << "[";
    for (const auto& op : ops) {
        sb << " " << op.toStringForLogging();
    }
    sb << " ]";
    return sb.str();
}

}  // namespace repl
}  // namespace mongo
