/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/mmap_v1/catalog/namespace_details.h"

#include <algorithm>
#include <list>

#include "mongo/base/counter.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/db.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_index.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/startup_test.h"

namespace mongo {

NamespaceDetails::NamespaceDetails(const DiskLoc& loc, bool capped) {
    static_assert(sizeof(NamespaceDetails::Extra) <= sizeof(NamespaceDetails),
                  "sizeof(NamespaceDetails::Extra) <= sizeof(NamespaceDetails)");

    /* be sure to initialize new fields here -- doesn't default to zeroes the way we use it */
    firstExtent = lastExtent = capExtent = loc;
    stats.datasize = stats.nrecords = 0;
    lastExtentSize = 0;
    nIndexes = 0;
    isCapped = capped;
    maxDocsInCapped = 0x7fffffff;  // no limit (value is for pre-v2.3.2 compatibility)
    paddingFactorOldDoNotUse = 1.0;
    systemFlagsOldDoNotUse = 0;
    userFlags = 0;
    capFirstNewRecord = DiskLoc();
    // Signal that we are on first allocation iteration through extents.
    capFirstNewRecord.setInvalid();
    // For capped case, signal that we are doing initial extent allocation.
    if (capped) {
        // WAS: cappedLastDelRecLastExtent().setInvalid();
        deletedListSmall[1].setInvalid();
    }
    verify(sizeof(_dataFileVersion) == 2);
    _dataFileVersion = 0;
    _indexFileVersion = 0;
    multiKeyIndexBits = 0;
    _reservedA = 0;
    _extraOffset = 0;
    indexBuildsInProgress = 0;
    memset(_reserved, 0, sizeof(_reserved));
}

NamespaceDetails::Extra* NamespaceDetails::allocExtra(OperationContext* txn,
                                                      StringData ns,
                                                      NamespaceIndex& ni,
                                                      int nindexessofar) {
    // Namespace details must always be changed under an exclusive DB lock
    const NamespaceString nss(ns);
    invariant(txn->lockState()->isDbLockedForMode(nss.db(), MODE_X));

    int i = (nindexessofar - NIndexesBase) / NIndexesExtra;
    verify(i >= 0 && i <= 1);

    Namespace fullns(ns);
    Namespace extrans(fullns.extraName(i));  // throws UserException if ns name too long

    massert(10350, "allocExtra: base ns missing?", this);
    massert(10351, "allocExtra: extra already exists", ni.details(extrans) == 0);

    Extra temp;
    temp.init();

    ni.add_ns(txn, extrans, reinterpret_cast<NamespaceDetails*>(&temp));
    Extra* e = reinterpret_cast<NamespaceDetails::Extra*>(ni.details(extrans));

    long ofs = e->ofsFrom(this);
    if (i == 0) {
        verify(_extraOffset == 0);
        *txn->recoveryUnit()->writing(&_extraOffset) = ofs;
        verify(extra() == e);
    } else {
        Extra* hd = extra();
        verify(hd->next(this) == 0);
        hd->setNext(txn, ofs);
    }
    return e;
}

IndexDetails& NamespaceDetails::idx(int idxNo, bool missingExpected) {
    if (idxNo < NIndexesBase) {
        IndexDetails& id = _indexes[idxNo];
        return id;
    }
    Extra* e = extra();
    if (!e) {
        if (missingExpected)
            throw MsgAssertionException(13283, "Missing Extra");
        massert(14045, "missing Extra", e);
    }
    int i = idxNo - NIndexesBase;
    if (i >= NIndexesExtra) {
        e = e->next(this);
        if (!e) {
            if (missingExpected)
                throw MsgAssertionException(14823, "missing extra");
            massert(14824, "missing Extra", e);
        }
        i -= NIndexesExtra;
    }
    return e->details[i];
}


const IndexDetails& NamespaceDetails::idx(int idxNo, bool missingExpected) const {
    if (idxNo < NIndexesBase) {
        const IndexDetails& id = _indexes[idxNo];
        return id;
    }
    const Extra* e = extra();
    if (!e) {
        if (missingExpected)
            throw MsgAssertionException(17421, "Missing Extra");
        massert(17422, "missing Extra", e);
    }
    int i = idxNo - NIndexesBase;
    if (i >= NIndexesExtra) {
        e = e->next(this);
        if (!e) {
            if (missingExpected)
                throw MsgAssertionException(17423, "missing extra");
            massert(17424, "missing Extra", e);
        }
        i -= NIndexesExtra;
    }
    return e->details[i];
}

NamespaceDetails::IndexIterator::IndexIterator(const NamespaceDetails* _d,
                                               bool includeBackgroundInProgress) {
    d = _d;
    i = 0;
    n = d->nIndexes;
    if (includeBackgroundInProgress)
        n += d->indexBuildsInProgress;
}

// must be called when renaming a NS to fix up extra
void NamespaceDetails::copyingFrom(OperationContext* txn,
                                   StringData thisns,
                                   NamespaceIndex& ni,
                                   NamespaceDetails* src) {
    _extraOffset = 0;  // we are a copy -- the old value is wrong.  fixing it up below.
    Extra* se = src->extra();
    int n = NIndexesBase;
    if (se) {
        Extra* e = allocExtra(txn, thisns, ni, n);
        while (1) {
            n += NIndexesExtra;
            e->copy(this, *se);
            se = se->next(src);
            if (se == 0)
                break;
            Extra* nxt = allocExtra(txn, thisns, ni, n);
            e->setNext(txn, nxt->ofsFrom(this));
            e = nxt;
        }
        verify(_extraOffset);
    }
}

NamespaceDetails* NamespaceDetails::writingWithoutExtra(OperationContext* txn) {
    return txn->recoveryUnit()->writing(this);
}


// XXX - this method should go away
NamespaceDetails* NamespaceDetails::writingWithExtra(OperationContext* txn) {
    for (Extra* e = extra(); e; e = e->next(this)) {
        txn->recoveryUnit()->writing(e);
    }
    return writingWithoutExtra(txn);
}

void NamespaceDetails::setMaxCappedDocs(OperationContext* txn, long long max) {
    massert(16499,
            "max in a capped collection has to be < 2^31 or -1",
            CollectionOptions::validMaxCappedDocs(&max));
    maxDocsInCapped = max;
}

/* ------------------------------------------------------------------------- */


int NamespaceDetails::_catalogFindIndexByName(OperationContext* txn,
                                              const Collection* coll,
                                              StringData name,
                                              bool includeBackgroundInProgress) const {
    IndexIterator i = ii(includeBackgroundInProgress);
    while (i.more()) {
        const BSONObj obj = coll->docFor(txn, i.next().info.toRecordId()).value();
        if (name == obj.getStringField("name"))
            return i.pos() - 1;
    }
    return -1;
}

void NamespaceDetails::Extra::setNext(OperationContext* txn, long ofs) {
    *txn->recoveryUnit()->writing(&_next) = ofs;
}

}  // namespace mongo
