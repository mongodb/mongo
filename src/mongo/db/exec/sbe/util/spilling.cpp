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
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/str.h"

namespace mongo {
namespace sbe {

void assertIgnorePrepareConflictsBehavior(OperationContext* opCtx) {
    tassert(5907502,
            "The operation must be ignoring conflicts and allowing writes or enforcing prepare "
            "conflicts entirely",
            opCtx->recoveryUnit()->getPrepareConflictBehavior() !=
                PrepareConflictBehavior::kIgnoreConflicts);
}


std::pair<RecordId, key_string::TypeBits> encodeKeyString(key_string::Builder& kb,
                                                          const value::MaterializedRow& value) {
    value.serializeIntoKeyString(kb);
    auto typeBits = kb.getTypeBits();
    auto rid = RecordId(kb.getBuffer(), kb.getSize());
    return {rid, typeBits};
}

key_string::Value decodeKeyString(const RecordId& rid, key_string::TypeBits typeBits) {
    auto rawKey = rid.getStr();
    key_string::Builder kb{key_string::Version::kLatestVersion};
    kb.resetFromBuffer(rawKey.rawData(), rawKey.size());
    kb.setTypeBits(typeBits);
    return kb.getValueCopy();
}

boost::optional<value::MaterializedRow> readFromRecordStore(OperationContext* opCtx,
                                                            RecordStore* rs,
                                                            const RecordId& rid) {
    RecordData record;
    if (rs->findRecord(opCtx, rid, &record)) {
        auto valueReader = BufReader(record.data(), record.size());
        return value::MaterializedRow::deserializeForSorter(valueReader, {});
    }
    return boost::none;
}

static int upsertToRecordStore(
    OperationContext* opCtx, RecordStore* rs, const RecordId& key, BufBuilder& buf, bool update) {

    assertIgnorePrepareConflictsBehavior(opCtx);

    WriteUnitOfWork wuow(opCtx);

    auto result = mongo::Status::OK();
    if (update) {
        result = rs->updateRecord(opCtx, key, buf.buf(), buf.len());
    } else {
        auto status = rs->insertRecord(opCtx, key, buf.buf(), buf.len(), Timestamp{});
        result = status.getStatus();
    }
    wuow.commit();
    if (!result.isOK()) {
        tasserted(5843600, str::stream() << "Failed to write to disk because " << result.reason());
        return 0;
    }
    return buf.len();
}

int upsertToRecordStore(OperationContext* opCtx,
                        RecordStore* rs,
                        const RecordId& key,
                        const value::MaterializedRow& val,
                        const key_string::TypeBits& typeBits,  // recover type of value.
                        bool update) {
    BufBuilder buf;
    val.serializeForSorter(buf);
    // Append the 'typeBits' to the end of the val's buffer so the 'key' can be reconstructed when
    // draining HashAgg.
    buf.appendBuf(typeBits.getBuffer(), typeBits.getSize());
    return upsertToRecordStore(opCtx, rs, key, buf, update);
}

int upsertToRecordStore(OperationContext* opCtx,
                        RecordStore* rs,
                        const RecordId& recordKey,
                        const value::MaterializedRow& key,
                        const value::MaterializedRow& val,
                        bool update) {
    BufBuilder buf;
    key.serializeForSorter(buf);
    val.serializeForSorter(buf);
    return upsertToRecordStore(opCtx, rs, recordKey, buf, update);
}

int upsertToRecordStore(OperationContext* opCtx,
                        RecordStore* rs,
                        const RecordId& key,
                        BufBuilder& buf,
                        const key_string::TypeBits& typeBits,  // recover type of value.
                        bool update) {
    // Append the 'typeBits' to the end of the val's buffer so the 'key' can be reconstructed when
    // draining HashAgg.
    buf.appendBuf(typeBits.getBuffer(), typeBits.getSize());
    return upsertToRecordStore(opCtx, rs, key, buf, update);
}
}  // namespace sbe
}  // namespace mongo
