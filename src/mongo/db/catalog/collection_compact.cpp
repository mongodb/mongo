// collection_compact.cpp

/**
*    Copyright (C) 2013 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/catalog/collection.h"

#include "mongo/base/counter.h"
#include "mongo/base/owned_pointer_map.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"
#include "mongo/util/touch_pages.h"

namespace mongo {

using std::endl;
using std::vector;

namespace {
BSONObj _compactAdjustIndexSpec(const BSONObj& oldSpec) {
    BSONObjBuilder b;
    BSONObj::iterator i(oldSpec);
    while (i.more()) {
        BSONElement e = i.next();
        if (str::equals(e.fieldName(), "v")) {
            // Drop any preexisting index version spec.  The default index version will
            // be used instead for the new index.
            continue;
        }
        if (str::equals(e.fieldName(), "background")) {
            // Create the new index in the foreground.
            continue;
        }
        // Pass the element through to the new index spec.
        b.append(e);
    }
    return b.obj();
}

class MyCompactAdaptor : public RecordStoreCompactAdaptor {
public:
    MyCompactAdaptor(Collection* collection, MultiIndexBlock* indexBlock)

        : _collection(collection), _multiIndexBlock(indexBlock) {}

    virtual bool isDataValid(const RecordData& recData) {
        return recData.toBson().valid();
    }

    virtual size_t dataSize(const RecordData& recData) {
        return recData.toBson().objsize();
    }

    virtual void inserted(const RecordData& recData, const RecordId& newLocation) {
        _multiIndexBlock->insert(recData.toBson(), newLocation);
    }

private:
    Collection* _collection;

    MultiIndexBlock* _multiIndexBlock;
};
}


StatusWith<CompactStats> Collection::compact(OperationContext* txn,
                                             const CompactOptions* compactOptions) {
    dassert(txn->lockState()->isCollectionLockedForMode(ns().toString(), MODE_X));

    DisableDocumentValidation validationDisabler(txn);

    if (!_recordStore->compactSupported())
        return StatusWith<CompactStats>(ErrorCodes::CommandNotSupported,
                                        str::stream()
                                            << "cannot compact collection with record store: "
                                            << _recordStore->name());

    if (_recordStore->compactsInPlace()) {
        CompactStats stats;
        Status status = _recordStore->compact(txn, NULL, compactOptions, &stats);
        if (!status.isOK())
            return StatusWith<CompactStats>(status);

        // Compact all indexes (not including unfinished indexes)
        IndexCatalog::IndexIterator ii(_indexCatalog.getIndexIterator(txn, false));
        while (ii.more()) {
            IndexDescriptor* descriptor = ii.next();
            IndexAccessMethod* index = _indexCatalog.getIndex(descriptor);

            LOG(1) << "compacting index: " << descriptor->toString();
            Status status = index->compact(txn);
            if (!status.isOK()) {
                error() << "failed to compact index: " << descriptor->toString();
                return status;
            }
        }

        return StatusWith<CompactStats>(stats);
    }

    if (_indexCatalog.numIndexesInProgress(txn))
        return StatusWith<CompactStats>(ErrorCodes::BadValue,
                                        "cannot compact when indexes in progress");

    vector<BSONObj> indexSpecs;
    {
        IndexCatalog::IndexIterator ii(_indexCatalog.getIndexIterator(txn, false));
        while (ii.more()) {
            IndexDescriptor* descriptor = ii.next();

            const BSONObj spec = _compactAdjustIndexSpec(descriptor->infoObj());
            const BSONObj key = spec.getObjectField("key");
            const Status keyStatus = validateKeyPattern(key);
            if (!keyStatus.isOK()) {
                return StatusWith<CompactStats>(
                    ErrorCodes::CannotCreateIndex,
                    str::stream() << "Cannot compact collection due to invalid index " << spec
                                  << ": "
                                  << keyStatus.reason()
                                  << " For more info see"
                                  << " http://dochub.mongodb.org/core/index-validation");
            }
            indexSpecs.push_back(spec);
        }
    }

    // Give a chance to be interrupted *before* we drop all indexes.
    txn->checkForInterrupt();

    {
        // note that the drop indexes call also invalidates all clientcursors for the namespace,
        // which is important and wanted here
        WriteUnitOfWork wunit(txn);
        log() << "compact dropping indexes" << endl;
        Status status = _indexCatalog.dropAllIndexes(txn, true);
        if (!status.isOK()) {
            return StatusWith<CompactStats>(status);
        }
        wunit.commit();
    }

    CompactStats stats;

    MultiIndexBlock indexer(txn, this);
    indexer.allowInterruption();
    indexer.ignoreUniqueConstraint();  // in compact we should be doing no checking

    Status status = indexer.init(indexSpecs);
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    MyCompactAdaptor adaptor(this, &indexer);

    status = _recordStore->compact(txn, &adaptor, compactOptions, &stats);
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    log() << "starting index commits";
    status = indexer.doneInserting();
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    {
        WriteUnitOfWork wunit(txn);
        indexer.commit();
        wunit.commit();
    }

    return StatusWith<CompactStats>(stats);
}

}  // namespace mongo
