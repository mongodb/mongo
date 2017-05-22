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

#include "mongo/db/catalog/collection_impl.h"

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
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;
using std::vector;

using IndexVersion = IndexDescriptor::IndexVersion;

namespace {
BSONObj _compactAdjustIndexSpec(const BSONObj& oldSpec) {
    BSONObjBuilder bob;

    for (auto&& indexSpecElem : oldSpec) {
        auto indexSpecElemFieldName = indexSpecElem.fieldNameStringData();
        if (IndexDescriptor::kIndexVersionFieldName == indexSpecElemFieldName) {
            IndexVersion indexVersion = static_cast<IndexVersion>(indexSpecElem.numberInt());
            if (IndexVersion::kV0 == indexVersion) {
                // We automatically upgrade v=0 indexes to v=1 indexes.
                bob.append(IndexDescriptor::kIndexVersionFieldName,
                           static_cast<int>(IndexVersion::kV1));
            } else {
                bob.append(IndexDescriptor::kIndexVersionFieldName, static_cast<int>(indexVersion));
            }
        } else if (IndexDescriptor::kBackgroundFieldName == indexSpecElemFieldName) {
            // Create the new index in the foreground.
            continue;
        } else {
            bob.append(indexSpecElem);
        }
    }

    return bob.obj();
}

class MyCompactAdaptor : public RecordStoreCompactAdaptor {
public:
    MyCompactAdaptor(Collection* collection, MultiIndexBlock* indexBlock)

        : _collection(collection), _multiIndexBlock(indexBlock) {}

    virtual bool isDataValid(const RecordData& recData) {
        // Use the latest BSON validation version. We allow compaction of collections containing
        // decimal data even if decimal is disabled.
        return recData.toBson().valid(BSONVersion::kLatest);
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


StatusWith<CompactStats> CollectionImpl::compact(OperationContext* opCtx,
                                                 const CompactOptions* compactOptions) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(ns().toString(), MODE_X));

    DisableDocumentValidation validationDisabler(opCtx);

    if (!_recordStore->compactSupported())
        return StatusWith<CompactStats>(ErrorCodes::CommandNotSupported,
                                        str::stream()
                                            << "cannot compact collection with record store: "
                                            << _recordStore->name());

    if (_recordStore->compactsInPlace()) {
        CompactStats stats;
        Status status = _recordStore->compact(opCtx, NULL, compactOptions, &stats);
        if (!status.isOK())
            return StatusWith<CompactStats>(status);

        // Compact all indexes (not including unfinished indexes)
        IndexCatalog::IndexIterator ii(_indexCatalog.getIndexIterator(opCtx, false));
        while (ii.more()) {
            IndexDescriptor* descriptor = ii.next();
            IndexAccessMethod* index = _indexCatalog.getIndex(descriptor);

            LOG(1) << "compacting index: " << descriptor->toString();
            Status status = index->compact(opCtx);
            if (!status.isOK()) {
                error() << "failed to compact index: " << descriptor->toString();
                return status;
            }
        }

        return StatusWith<CompactStats>(stats);
    }

    if (_indexCatalog.numIndexesInProgress(opCtx))
        return StatusWith<CompactStats>(ErrorCodes::BadValue,
                                        "cannot compact when indexes in progress");

    vector<BSONObj> indexSpecs;
    {
        IndexCatalog::IndexIterator ii(_indexCatalog.getIndexIterator(opCtx, false));
        while (ii.more()) {
            IndexDescriptor* descriptor = ii.next();

            const BSONObj spec = _compactAdjustIndexSpec(descriptor->infoObj());
            const BSONObj key = spec.getObjectField("key");
            const Status keyStatus =
                index_key_validate::validateKeyPattern(key, descriptor->version());
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
    opCtx->checkForInterrupt();

    {
        // note that the drop indexes call also invalidates all clientcursors for the namespace,
        // which is important and wanted here
        WriteUnitOfWork wunit(opCtx);
        log() << "compact dropping indexes";
        _indexCatalog.dropAllIndexes(opCtx, true);
        wunit.commit();
    }

    CompactStats stats;

    MultiIndexBlock indexer(opCtx, _this);
    indexer.allowInterruption();
    indexer.ignoreUniqueConstraint();  // in compact we should be doing no checking

    Status status = indexer.init(indexSpecs).getStatus();
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    MyCompactAdaptor adaptor(_this, &indexer);

    status = _recordStore->compact(opCtx, &adaptor, compactOptions, &stats);
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    log() << "starting index commits";
    status = indexer.doneInserting();
    if (!status.isOK())
        return StatusWith<CompactStats>(status);

    {
        WriteUnitOfWork wunit(opCtx);
        indexer.commit();
        wunit.commit();
    }

    return StatusWith<CompactStats>(stats);
}

}  // namespace mongo
