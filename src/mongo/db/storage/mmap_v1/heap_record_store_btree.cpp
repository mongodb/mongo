// heap_record_store_btree.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/heap_record_store_btree.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

RecordData HeapRecordStoreBtree::dataFor(OperationContext* txn, const RecordId& loc) const {
    Records::const_iterator it = _records.find(loc);
    invariant(it != _records.end());
    const MmapV1RecordHeader& rec = it->second;

    return RecordData(rec.data.get(), rec.dataSize);
}

bool HeapRecordStoreBtree::findRecord(OperationContext* txn,
                                      const RecordId& loc,
                                      RecordData* out) const {
    Records::const_iterator it = _records.find(loc);
    if (it == _records.end())
        return false;
    const MmapV1RecordHeader& rec = it->second;
    *out = RecordData(rec.data.get(), rec.dataSize);
    return true;
}

void HeapRecordStoreBtree::deleteRecord(OperationContext* txn, const RecordId& loc) {
    invariant(_records.erase(loc) == 1);
}

StatusWith<RecordId> HeapRecordStoreBtree::insertRecord(OperationContext* txn,
                                                        const char* data,
                                                        int len,
                                                        bool enforceQuota) {
    MmapV1RecordHeader rec(len);
    memcpy(rec.data.get(), data, len);

    const RecordId loc = allocateLoc();
    _records[loc] = rec;

    HeapRecordStoreBtreeRecoveryUnit::notifyInsert(txn, this, loc);

    return StatusWith<RecordId>(loc);
}

Status HeapRecordStoreBtree::insertRecordsWithDocWriter(OperationContext* txn,
                                                        const DocWriter* const* docs,
                                                        size_t nDocs,
                                                        RecordId* idsOut) {
    // This class is only for unit tests of the mmapv1 btree code and this is how it is called.
    // If that ever changes, this class will need to be fixed.
    invariant(nDocs == 1);
    invariant(idsOut);

    MmapV1RecordHeader rec(docs[0]->documentSize());
    docs[0]->writeDocument(rec.data.get());

    const RecordId loc = allocateLoc();
    _records[loc] = rec;
    *idsOut = loc;

    HeapRecordStoreBtreeRecoveryUnit::notifyInsert(txn, this, loc);

    return Status::OK();
}

RecordId HeapRecordStoreBtree::allocateLoc() {
    const int64_t id = _nextId++;
    // This is a hack, but both the high and low order bits of RecordId offset must be 0, and the
    // file must fit in 23 bits. This gives us a total of 30 + 23 == 53 bits.
    invariant(id < (1LL << 53));
    RecordId dl(int(id >> 30), int((id << 1) & ~(1 << 31)));
    invariant((dl.repr() & 0x1) == 0);
    return dl;
}

Status HeapRecordStoreBtree::touch(OperationContext* txn, BSONObjBuilder* output) const {
    // not currently called from the tests, but called from btree_logic.h
    return Status::OK();
}

// ---------------------------

void HeapRecordStoreBtreeRecoveryUnit::commitUnitOfWork() {
    _insertions.clear();
    _mods.clear();
}

void HeapRecordStoreBtreeRecoveryUnit::abortUnitOfWork() {
    // reverse in case we write same area twice
    for (size_t i = _mods.size(); i > 0; i--) {
        ModEntry& e = _mods[i - 1];
        memcpy(e.data, e.old.get(), e.len);
    }

    invariant(_insertions.size() == 0);  // todo
}

void* HeapRecordStoreBtreeRecoveryUnit::writingPtr(void* data, size_t len) {
    ModEntry e = {data, len, boost::shared_array<char>(new char[len])};
    memcpy(e.old.get(), data, len);
    _mods.push_back(e);
    return data;
}

void HeapRecordStoreBtreeRecoveryUnit::notifyInsert(HeapRecordStoreBtree* rs, const RecordId& loc) {
    InsertEntry e = {rs, loc};
    _insertions.push_back(e);
}

void HeapRecordStoreBtreeRecoveryUnit::notifyInsert(OperationContext* ctx,
                                                    HeapRecordStoreBtree* rs,
                                                    const RecordId& loc) {
    if (!ctx)
        return;

    // This dynamic_cast has semantics, should change ideally.
    HeapRecordStoreBtreeRecoveryUnit* ru =
        dynamic_cast<HeapRecordStoreBtreeRecoveryUnit*>(ctx->recoveryUnit());

    if (!ru)
        return;

    ru->notifyInsert(rs, loc);
}


}  // namespace mongo
