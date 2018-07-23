// biggie_record_store.cpp

/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


// ALERT: need to remodify db.cpp to actually create an fcv on line about 422 (!storageGlobalParams.readOnly && (storageGlobalParams.engine != "devnull");)
// once this stuff actually gets implemented!!!

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/biggie/biggie_record_store.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/biggie/biggie_store.h"
#include "mongo/db/storage/biggie/store.h"
#include "mongo/stdx/memory.h"

#include <cstring>
#include <memory>
#include <utility>

// #include "mongo/db/jsobj.h"
// #include "mongo/db/namespace_string.h"
// #inclu de "mongo/db/storage/oplog_hack.h"
// #include "mongo/db/storage/recovery_unit.h"
// #include "mongo/util/log.h"
// #include "mongo/util/mongoutils/str.h"
// #include "mongo/util/unowned_ptr.h"

namespace mongo {

BiggieRecordStore::BiggieRecordStore(StringData ns,
                                     std::shared_ptr<BiggieStore> data,
                                     bool isCapped,
                                     int64_t cappedMaxSize,
                                     int64_t cappedMaxDocs,
                                     CappedCallback* cappedCallback)
    : RecordStore(ns),
      _data(data),
      _isCapped(isCapped),
      _cappedMaxSize(cappedMaxSize),
      _cappedMaxDocs(cappedMaxDocs),
      _cappedCallback(cappedCallback) {
        _dummy = BSON("_id" << 1);
    }

const char* BiggieRecordStore::name() const {
    return "biggie";
}

long long BiggieRecordStore::dataSize(OperationContext* opCtx) const {
    // TODO: Understand what this should return
    return -1;
}

long long BiggieRecordStore::numRecords(OperationContext* opCtx) const {
    // TODO: Return a real answer here
    return 0; //// (long long)_data->size();
}

bool BiggieRecordStore::isCapped() const {
    return _isCapped;
}
int64_t BiggieRecordStore::storageSize(OperationContext* opCtx,
                                       BSONObjBuilder* extraInfo,
                                       int infoLevel) const {
    return 100;  //? Is this implemented here, or by BiggieStore
}

RecordData BiggieRecordStore::dataFor(OperationContext* opCtx, const RecordId& loc) const {
    // TODO : needs to be changed
    return RecordData(_dummy.objdata(), _dummy.objsize());
}

bool BiggieRecordStore::findRecord(OperationContext* opCtx,
                                   const RecordId& loc,
                                   RecordData* rd) const {
    // TODO: We should probably find a record
    // can't do this without a find
    // Key key(&(loc.repr()), 8);
    return false;
}
void BiggieRecordStore::deleteRecord(OperationContext* opCtx, const RecordId&) {
    // TODO: need to iterate through our store and delete the record
    return;
}

StatusWith<RecordId> BiggieRecordStore::insertRecord(
    OperationContext* opCtx, const char* data, int len, Timestamp, bool enforceQuota) {
    size_t num_chunks = 64 / sizeof(uint8_t);
    uint8_t* key_ptr = (uint8_t*)std::malloc(num_chunks);
    uint64_t thisRecordId = ++nextRecordId;
    std::memcpy(key_ptr, &thisRecordId, num_chunks);

    Key key(key_ptr, num_chunks);
    Store::Value v(key, std::string(data, len));
    // _data->insert(std::move(v));

    RecordId rID(thisRecordId);
    return StatusWith<RecordId>(rID);
}

Status BiggieRecordStore::insertRecordsWithDocWriter(OperationContext* opCtx,
                                                     const DocWriter* const* docs,
                                                     const Timestamp*,
                                                     size_t nDocs,
                                                     RecordId* idsOut) {
    // TODO: Implement
    return Status::OK();
}

Status BiggieRecordStore::updateRecord(OperationContext* opCtx,
                                       const RecordId& oldLocation,
                                       const char* data,
                                       int len,
                                       bool enforceQuota,
                                       UpdateNotifier* notifier) {
    // TODO Implement
    return Status::OK();
}

bool BiggieRecordStore::updateWithDamagesSupported() const {
    // TODO : Implement
    return false;
}

StatusWith<RecordData> BiggieRecordStore::updateWithDamages(
    OperationContext* opCtx,
    const RecordId& loc,
    const RecordData& oldRec,
    const char* damageSource,
    const mutablebson::DamageVector& damages) {
    // TODO: Implement
    return StatusWith<RecordData>(oldRec);
}

std::unique_ptr<SeekableRecordCursor> BiggieRecordStore::getCursor(OperationContext* opCtx,
                                                                   bool forward) const {
    // TODO : implement
    return std::make_unique<Cursor>(opCtx, *this);
}

Status BiggieRecordStore::truncate(OperationContext* opCtx) {
    // TODO : implement
    return Status::OK();
}

void BiggieRecordStore::cappedTruncateAfter(OperationContext* opCtx, RecordId end, bool inclusive) {
    // TODO : implement
}

Status BiggieRecordStore::validate(OperationContext* opCtx,
                                   ValidateCmdLevel level,
                                   ValidateAdaptor* adaptor,
                                   ValidateResults* results,
                                   BSONObjBuilder* output) {
    // TODO : implement
    return Status::OK();
}

void BiggieRecordStore::appendCustomStats(OperationContext* opCtx,
                                          BSONObjBuilder* result,
                                          double scale) const {
    // TODO: Implement
}

Status BiggieRecordStore::touch(OperationContext* opCtx, BSONObjBuilder* output) const {
    // TODO : implement
    return Status::OK();
}

void BiggieRecordStore::waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx) const {
    // TODO : implement
}

void BiggieRecordStore::updateStatsAfterRepair(OperationContext* opCtx,
                                               long long numRecords,
                                               long long dataSize) {
    // TODO: Implement
}


BiggieRecordStore::Cursor::Cursor(OperationContext* opCtx, const BiggieRecordStore& rs) {}

boost::optional<Record> BiggieRecordStore::Cursor::next() {
    return boost::none;
}

boost::optional<Record> BiggieRecordStore::Cursor::seekExact(const RecordId& id) {
    return boost::none;
}

void BiggieRecordStore::Cursor::save() {}

void BiggieRecordStore::Cursor::saveUnpositioned() {}

bool BiggieRecordStore::Cursor::restore() {
    return false;
}

void BiggieRecordStore::Cursor::detachFromOperationContext() {}
void BiggieRecordStore::Cursor::reattachToOperationContext(OperationContext* opCtx) {}


}  // namespace mongo
