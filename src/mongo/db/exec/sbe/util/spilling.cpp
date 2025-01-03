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

#include "mongo/db/exec/sbe/util/spilling.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {

namespace {
/* We don't need to retry write conflicts in this class but when WiredTiger is low on cache
 * we do need to retry the StorageUnavailable exceptions.
 */
template <typename F>
static void storageUnavailableRetry(OperationContext* opCtx,
                                    StringData opStr,
                                    F&& f,
                                    boost::optional<size_t> retryLimit = boost::none) {
    // writeConflictRetry already implements a retryBackoff for storage unavailable.
    writeConflictRetry(opCtx, opStr, NamespaceString::kEmpty, f, retryLimit);
}
}  // namespace

void assertIgnorePrepareConflictsBehavior(OperationContext* opCtx) {
    tassert(5907502,
            "The operation must be ignoring conflicts and allowing writes or enforcing prepare "
            "conflicts entirely",
            shard_role_details::getRecoveryUnit(opCtx)->getPrepareConflictBehavior() !=
                PrepareConflictBehavior::kIgnoreConflicts);
}

std::pair<RecordId, key_string::TypeBits> encodeKeyString(key_string::Builder& kb,
                                                          const value::MaterializedRow& value) {
    value.serializeIntoKeyString(kb);
    auto typeBits = kb.getTypeBits();
    auto rid = RecordId(kb.getView());
    return {rid, typeBits};
}

key_string::Value decodeKeyString(const RecordId& rid, key_string::TypeBits typeBits) {
    key_string::Builder kb{key_string::Version::kLatestVersion};
    kb.resetFromBuffer(rid.getStr());
    kb.setTypeBits(typeBits);
    return kb.getValueCopy();
}

SpillingStore::SpillingStore(OperationContext* opCtx, KeyFormat format) {
    _recordStore =
        opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(opCtx, format);

    _spillingUnit = std::unique_ptr<RecoveryUnit>(
        opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit());
    _spillingUnit->setCacheMaxWaitTimeout(Milliseconds(internalQuerySpillingMaxWaitTimeout.load()));
    _spillingState = WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork;
}

SpillingStore::~SpillingStore() {}

int SpillingStore::upsertToRecordStore(OperationContext* opCtx,
                                       const RecordId& recordKey,
                                       const value::MaterializedRow& key,
                                       const value::MaterializedRow& val,
                                       bool update) {
    BufBuilder buf;
    key.serializeForSorter(buf);
    val.serializeForSorter(buf);
    return upsertToRecordStore(opCtx, recordKey, buf, update);
}

int SpillingStore::upsertToRecordStore(
    OperationContext* opCtx,
    const RecordId& key,
    const value::MaterializedRow& val,
    const key_string::TypeBits& typeBits,  // recover type of value.
    bool update) {
    BufBuilder bufValue;
    val.serializeForSorter(bufValue);
    // Append the 'typeBits' to the end of the val's buffer so the 'key' can be reconstructed when
    // draining HashAgg.
    bufValue.appendBuf(typeBits.getBuffer(), typeBits.getSize());

    return upsertToRecordStore(opCtx, key, bufValue, update);
}

int SpillingStore::upsertToRecordStore(
    OperationContext* opCtx,
    const RecordId& key,
    BufBuilder& buf,
    const key_string::TypeBits& typeBits,  // recover type of value.
    bool update) {
    // Append the 'typeBits' to the end of the val's buffer so the 'key' can be reconstructed when
    // draining HashAgg.
    buf.appendBuf(typeBits.getBuffer(), typeBits.getSize());

    return upsertToRecordStore(opCtx, key, buf, update);
}

int SpillingStore::upsertToRecordStore(OperationContext* opCtx,
                                       const RecordId& key,
                                       BufBuilder& buf,
                                       bool update) {
    assertIgnorePrepareConflictsBehavior(opCtx);

    switchToSpilling(opCtx);
    ON_BLOCK_EXIT([&] { switchToOriginal(opCtx); });
    auto result = mongo::Status::OK();

    storageUnavailableRetry(opCtx, "SpillingStore::upsertToRecordStore", [&] {
        WriteUnitOfWork wuow(opCtx);
        if (update) {
            result = rs()->updateRecord(opCtx, key, buf.buf(), buf.len());
        } else {
            auto status = rs()->insertRecord(opCtx, key, buf.buf(), buf.len(), Timestamp{});
            result = status.getStatus();
        }
        wuow.commit();
    });

    if (!result.isOK()) {
        tasserted(5843600, str::stream() << "Failed to write to disk because " << result.reason());
        return 0;
    }
    return buf.len();
}

Status SpillingStore::insertRecords(OperationContext* opCtx,
                                    std::vector<Record>* inOutRecords,
                                    const std::vector<Timestamp>& timestamps) {
    assertIgnorePrepareConflictsBehavior(opCtx);

    switchToSpilling(opCtx);
    ON_BLOCK_EXIT([&] { switchToOriginal(opCtx); });
    auto status = Status::OK();

    storageUnavailableRetry(opCtx, "SpillingStore::insertRecords", [&] {
        WriteUnitOfWork wuow(opCtx);
        status = rs()->insertRecords(opCtx, inOutRecords, timestamps);
        wuow.commit();
    });
    return status;
}

boost::optional<value::MaterializedRow> SpillingStore::readFromRecordStore(OperationContext* opCtx,
                                                                           const RecordId& rid) {
    switchToSpilling(opCtx);
    ON_BLOCK_EXIT([&] { switchToOriginal(opCtx); });

    RecordData record;
    bool found = false;
    // Because we impose a timeout for storage engine operations, we need to handle errors and retry
    // reads too.
    storageUnavailableRetry(opCtx, "SpillingStore::readFromRecordStore", [&] {
        found = rs()->findRecord(opCtx, rid, &record);
    });

    if (found) {
        auto valueReader = BufReader(record.data(), record.size());
        return value::MaterializedRow::deserializeForSorter(valueReader, {});
    }
    return boost::none;
}

bool SpillingStore::findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* out) {
    switchToSpilling(opCtx);
    ON_BLOCK_EXIT([&] { switchToOriginal(opCtx); });
    bool found = false;
    // Because we impose a timeout for storage engine operations, we need to handle errors and retry
    // reads too.
    storageUnavailableRetry(
        opCtx, "SpillingStore::findRecord", [&] { found = rs()->findRecord(opCtx, loc, out); });
    return found;
}

void SpillingStore::switchToSpilling(OperationContext* opCtx) {
    invariant(!_originalUnit);
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    _originalUnit = shard_role_details::releaseRecoveryUnit(opCtx);
    _originalState =
        shard_role_details::setRecoveryUnit(opCtx, std::move(_spillingUnit), _spillingState);
}
void SpillingStore::switchToOriginal(OperationContext* opCtx) {
    invariant(!_spillingUnit);
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    _spillingUnit = shard_role_details::releaseRecoveryUnit(opCtx);
    _spillingState =
        shard_role_details::setRecoveryUnit(opCtx, std::move(_originalUnit), _originalState);
    invariant(!(_spillingUnit->getState() == RecoveryUnit::State::kInactiveInUnitOfWork ||
                _spillingUnit->getState() == RecoveryUnit::State::kActive));
}

void SpillingStore::saveState() {
    _spillingUnit->abandonSnapshot();
}
void SpillingStore::restoreState() {
    // We do not have to do anything.
}

}  // namespace sbe
}  // namespace mongo
