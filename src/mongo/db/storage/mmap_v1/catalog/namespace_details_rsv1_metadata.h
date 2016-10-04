// namespace_details_rsv1_metadata.h

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

#pragma once

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_base.h"

namespace mongo {

class RecordStore;

/*
 * NOTE: NamespaceDetails will become a struct
 *      all dur, etc... will move here
 */
class NamespaceDetailsRSV1MetaData : public RecordStoreV1MetaData {
public:
    explicit NamespaceDetailsRSV1MetaData(StringData ns, NamespaceDetails* details);

    virtual ~NamespaceDetailsRSV1MetaData() {}

    virtual const DiskLoc& capExtent() const;
    virtual void setCapExtent(OperationContext* txn, const DiskLoc& loc);

    virtual const DiskLoc& capFirstNewRecord() const;
    virtual void setCapFirstNewRecord(OperationContext* txn, const DiskLoc& loc);

    virtual bool capLooped() const;

    virtual long long dataSize() const;
    virtual long long numRecords() const;

    virtual void incrementStats(OperationContext* txn,
                                long long dataSizeIncrement,
                                long long numRecordsIncrement);

    virtual void setStats(OperationContext* txn, long long dataSize, long long numRecords);

    virtual DiskLoc deletedListEntry(int bucket) const;
    virtual void setDeletedListEntry(OperationContext* txn, int bucket, const DiskLoc& loc);

    virtual DiskLoc deletedListLegacyGrabBag() const;
    virtual void setDeletedListLegacyGrabBag(OperationContext* txn, const DiskLoc& loc);

    virtual void orphanDeletedList(OperationContext* txn);

    virtual const DiskLoc& firstExtent(OperationContext* txn) const;
    virtual void setFirstExtent(OperationContext* txn, const DiskLoc& loc);

    virtual const DiskLoc& lastExtent(OperationContext* txn) const;
    virtual void setLastExtent(OperationContext* txn, const DiskLoc& loc);

    virtual bool isCapped() const;

    virtual bool isUserFlagSet(int flag) const;
    virtual int userFlags() const;
    virtual bool setUserFlag(OperationContext* txn, int flag);
    virtual bool clearUserFlag(OperationContext* txn, int flag);
    virtual bool replaceUserFlags(OperationContext* txn, int flags);

    virtual int lastExtentSize(OperationContext* txn) const;
    virtual void setLastExtentSize(OperationContext* txn, int newMax);

    virtual long long maxCappedDocs() const;

private:
    std::string _ns;
    NamespaceDetails* _details;
    RecordStore* _namespaceRecordStore;
};
}
