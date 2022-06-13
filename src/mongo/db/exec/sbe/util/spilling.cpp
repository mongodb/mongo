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

namespace mongo {
namespace sbe {

void assertIgnorePrepareConflictsBehavior(OperationContext* opCtx) {
    tassert(5907502,
            "The operation must be ignoring conflicts and allowing writes or enforcing prepare "
            "conflicts entirely",
            opCtx->recoveryUnit()->getPrepareConflictBehavior() !=
                PrepareConflictBehavior::kIgnoreConflicts);
}


std::pair<RecordId, KeyString::TypeBits> encodeKeyString(KeyString::Builder& kb,
                                                         const value::MaterializedRow& value) {
    value.serializeIntoKeyString(kb);
    auto typeBits = kb.getTypeBits();
    auto rid = RecordId(kb.getBuffer(), kb.getSize());
    return {rid, typeBits};
}

KeyString::Value decodeKeyString(const RecordId& rid, KeyString::TypeBits typeBits) {
    auto rawKey = rid.getStr();
    KeyString::Builder kb{KeyString::Version::kLatestVersion};
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

int upsertToRecordStore(OperationContext* opCtx,
                        RecordStore* rs,
                        const RecordId& key,
                        const value::MaterializedRow& val,
                        const KeyString::TypeBits& typeBits,  // recover type of value.
                        bool update) {
    BufBuilder bufValue;
    val.serializeForSorter(bufValue);
    return upsertToRecordStore(opCtx, rs, key, bufValue, typeBits, update);
}

int upsertToRecordStore(OperationContext* opCtx,
                        RecordStore* rs,
                        const RecordId& key,
                        BufBuilder& buf,
                        const KeyString::TypeBits& typeBits,  // recover type of value.
                        bool update) {

    // Append the 'typeBits' to the end of the val's buffer so the 'key' can be reconstructed when
    // draining HashAgg.
    buf.appendBuf(typeBits.getBuffer(), typeBits.getSize());

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
}  // namespace sbe
}  // namespace mongo
