// namespace_details_rsv1_metadata.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
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

#include "mongo/db/storage/mmap_v1/catalog/namespace_details_rsv1_metadata.h"


#include "mongo/db/operation_context.h"

namespace mongo {

using std::unique_ptr;
using std::numeric_limits;

static_assert(RecordStoreV1Base::Buckets ==
                  NamespaceDetails::SmallBuckets + NamespaceDetails::LargeBuckets,
              "RecordStoreV1Base::Buckets == NamespaceDetails::SmallBuckets + "
              "NamespaceDetails::LargeBuckets");

NamespaceDetailsRSV1MetaData::NamespaceDetailsRSV1MetaData(StringData ns, NamespaceDetails* details)
    : _ns(ns.toString()), _details(details) {}

const DiskLoc& NamespaceDetailsRSV1MetaData::capExtent() const {
    return _details->capExtent;
}

void NamespaceDetailsRSV1MetaData::setCapExtent(OperationContext* txn, const DiskLoc& loc) {
    *txn->recoveryUnit()->writing(&_details->capExtent) = loc;
}

const DiskLoc& NamespaceDetailsRSV1MetaData::capFirstNewRecord() const {
    return _details->capFirstNewRecord;
}

void NamespaceDetailsRSV1MetaData::setCapFirstNewRecord(OperationContext* txn, const DiskLoc& loc) {
    *txn->recoveryUnit()->writing(&_details->capFirstNewRecord) = loc;
}

bool NamespaceDetailsRSV1MetaData::capLooped() const {
    return _details->capFirstNewRecord.isValid();
}

long long NamespaceDetailsRSV1MetaData::dataSize() const {
    return _details->stats.datasize;
}
long long NamespaceDetailsRSV1MetaData::numRecords() const {
    return _details->stats.nrecords;
}

void NamespaceDetailsRSV1MetaData::incrementStats(OperationContext* txn,
                                                  long long dataSizeIncrement,
                                                  long long numRecordsIncrement) {
    // durability todo : this could be a bit annoying / slow to record constantly
    NamespaceDetails::Stats* s = txn->recoveryUnit()->writing(&_details->stats);
    s->datasize += dataSizeIncrement;
    s->nrecords += numRecordsIncrement;
}

void NamespaceDetailsRSV1MetaData::setStats(OperationContext* txn,
                                            long long dataSize,
                                            long long numRecords) {
    NamespaceDetails::Stats* s = txn->recoveryUnit()->writing(&_details->stats);
    s->datasize = dataSize;
    s->nrecords = numRecords;
}

DiskLoc NamespaceDetailsRSV1MetaData::deletedListEntry(int bucket) const {
    invariant(bucket >= 0 && bucket < RecordStoreV1Base::Buckets);
    const DiskLoc head = (bucket < NamespaceDetails::SmallBuckets)
        ? _details->deletedListSmall[bucket]
        : _details->deletedListLarge[bucket - NamespaceDetails::SmallBuckets];

    if (head == DiskLoc(0, 0)) {
        // This will happen the first time we use a "large" bucket since they were previously
        // zero-initialized.
        return DiskLoc();
    }

    return head;
}

void NamespaceDetailsRSV1MetaData::setDeletedListEntry(OperationContext* txn,
                                                       int bucket,
                                                       const DiskLoc& loc) {
    DiskLoc* head = (bucket < NamespaceDetails::SmallBuckets)
        ? &_details->deletedListSmall[bucket]
        : &_details->deletedListLarge[bucket - NamespaceDetails::SmallBuckets];
    *txn->recoveryUnit()->writing(head) = loc;
}

DiskLoc NamespaceDetailsRSV1MetaData::deletedListLegacyGrabBag() const {
    return _details->deletedListLegacyGrabBag;
}

void NamespaceDetailsRSV1MetaData::setDeletedListLegacyGrabBag(OperationContext* txn,
                                                               const DiskLoc& loc) {
    *txn->recoveryUnit()->writing(&_details->deletedListLegacyGrabBag) = loc;
}

void NamespaceDetailsRSV1MetaData::orphanDeletedList(OperationContext* txn) {
    for (int i = 0; i < RecordStoreV1Base::Buckets; i++) {
        setDeletedListEntry(txn, i, DiskLoc());
    }
    setDeletedListLegacyGrabBag(txn, DiskLoc());
}

const DiskLoc& NamespaceDetailsRSV1MetaData::firstExtent(OperationContext* txn) const {
    return _details->firstExtent;
}

void NamespaceDetailsRSV1MetaData::setFirstExtent(OperationContext* txn, const DiskLoc& loc) {
    *txn->recoveryUnit()->writing(&_details->firstExtent) = loc;
}

const DiskLoc& NamespaceDetailsRSV1MetaData::lastExtent(OperationContext* txn) const {
    return _details->lastExtent;
}

void NamespaceDetailsRSV1MetaData::setLastExtent(OperationContext* txn, const DiskLoc& loc) {
    *txn->recoveryUnit()->writing(&_details->lastExtent) = loc;
}

bool NamespaceDetailsRSV1MetaData::isCapped() const {
    return _details->isCapped;
}

bool NamespaceDetailsRSV1MetaData::isUserFlagSet(int flag) const {
    return _details->userFlags & flag;
}

int NamespaceDetailsRSV1MetaData::userFlags() const {
    return _details->userFlags;
}

bool NamespaceDetailsRSV1MetaData::setUserFlag(OperationContext* txn, int flag) {
    if ((_details->userFlags & flag) == flag)
        return false;

    txn->recoveryUnit()->writingInt(_details->userFlags) |= flag;
    return true;
}

bool NamespaceDetailsRSV1MetaData::clearUserFlag(OperationContext* txn, int flag) {
    if ((_details->userFlags & flag) == 0)
        return false;

    txn->recoveryUnit()->writingInt(_details->userFlags) &= ~flag;
    return true;
}

bool NamespaceDetailsRSV1MetaData::replaceUserFlags(OperationContext* txn, int flags) {
    if (_details->userFlags == flags)
        return false;

    txn->recoveryUnit()->writingInt(_details->userFlags) = flags;
    return true;
}

int NamespaceDetailsRSV1MetaData::lastExtentSize(OperationContext* txn) const {
    return _details->lastExtentSize;
}

void NamespaceDetailsRSV1MetaData::setLastExtentSize(OperationContext* txn, int newMax) {
    if (_details->lastExtentSize == newMax)
        return;
    txn->recoveryUnit()->writingInt(_details->lastExtentSize) = newMax;
}

long long NamespaceDetailsRSV1MetaData::maxCappedDocs() const {
    invariant(_details->isCapped);
    if (_details->maxDocsInCapped == 0x7fffffff)
        return numeric_limits<long long>::max();
    return _details->maxDocsInCapped;
}
}
