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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/str.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {

namespace {

[[nodiscard]] Lock::GlobalLock acquireLock(OperationContext* opCtx) {
    return Lock::GlobalLock(
        opCtx,
        LockMode::MODE_IS,
        Date_t::max(),
        Lock::InterruptBehavior::kThrow,
        Lock::GlobalLockOptions{.skipFlowControlTicket = true, .skipRSTLLock = true});
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
    auto lk = acquireLock(opCtx);
    _spillTable = opCtx->getServiceContext()->getStorageEngine()->makeSpillTable(
        opCtx, format, internalQuerySpillingMinAvailableDiskSpaceBytes.load());
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

    auto result = [&] {
        auto lk = acquireLock(opCtx);
        if (update) {
            return _spillTable->updateRecord(opCtx, key, buf.buf(), buf.len());
        }
        std::vector<Record> records = {{key, {buf.buf(), buf.len()}}};
        return _spillTable->insertRecords(opCtx, &records);
    }();

    uassertStatusOK(result);
    return buf.len();
}

Status SpillingStore::insertRecords(OperationContext* opCtx, std::vector<Record>* inOutRecords) {
    assertIgnorePrepareConflictsBehavior(opCtx);

    auto lk = acquireLock(opCtx);
    return _spillTable->insertRecords(opCtx, inOutRecords);
}

boost::optional<value::MaterializedRow> SpillingStore::readFromRecordStore(OperationContext* opCtx,
                                                                           const RecordId& rid) {
    RecordData record;
    auto found = [&] {
        auto lk = acquireLock(opCtx);
        return _spillTable->findRecord(opCtx, rid, &record);
    }();

    if (found) {
        auto valueReader = BufReader(record.data(), record.size());
        return value::MaterializedRow::deserializeForSorter(valueReader, {});
    }
    return boost::none;
}

bool SpillingStore::findRecord(OperationContext* opCtx, const RecordId& loc, RecordData* out) {
    auto lk = acquireLock(opCtx);
    return _spillTable->findRecord(opCtx, loc, out);
}

int64_t SpillingStore::storageSize(OperationContext* opCtx) {
    auto lk = acquireLock(opCtx);
    return _spillTable->storageSize();
}

void SpillingStore::updateSpillStorageStatsForOperation(OperationContext* opCtx) {
    CurOp::get(opCtx)->updateSpillStorageStats(
        _spillTable->computeOperationStatisticsSinceLastCall());
}

}  // namespace sbe
}  // namespace mongo
