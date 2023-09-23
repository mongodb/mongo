/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/repl/collection_bulk_loader_impl.h"

#include <cstddef>
#include <exception>
#include <utility>


#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/duration.h"
#include "mongo/util/shared_buffer_fragment.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

CollectionBulkLoaderImpl::CollectionBulkLoaderImpl(ServiceContext::UniqueClient client,
                                                   ServiceContext::UniqueOperationContext opCtx,
                                                   const NamespaceString& nss,
                                                   const BSONObj& idIndexSpec)
    : _client{std::move(client)},
      _opCtx{std::move(opCtx)},
      _acquisition(
          acquireCollectionForLocalCatalogOnlyWithPotentialDataLoss(_opCtx.get(), nss, MODE_X)),
      _nss{nss},
      _idIndexBlock(std::make_unique<MultiIndexBlock>()),
      _secondaryIndexesBlock(std::make_unique<MultiIndexBlock>()),
      _idIndexSpec(idIndexSpec.getOwned()) {
    invariant(_opCtx);
}

CollectionBulkLoaderImpl::~CollectionBulkLoaderImpl() {
    AlternativeClientRegion acr(_client);
    DESTRUCTOR_GUARD({ _releaseResources(); })
}

Status CollectionBulkLoaderImpl::init(const std::vector<BSONObj>& secondaryIndexSpecs) {
    return _runTaskReleaseResourcesOnFailure([&secondaryIndexSpecs, this]() -> Status {
        // This method is called during initial sync of a replica set member, so we can safely tell
        // the index builders to build in the foreground instead of using the hybrid approach. The
        // member won't be available to be queried by anyone until it's caught up with the primary.
        // The only reason to do this is to force the index document insertion to not yield the
        // locks as yielding a MODE_X/MODE_S lock isn't allowed.
        _secondaryIndexesBlock->setIndexBuildMethod(IndexBuildMethod::kForeground);
        _idIndexBlock->setIndexBuildMethod(IndexBuildMethod::kForeground);
        return writeConflictRetry(
            _opCtx.get(),
            "CollectionBulkLoader::init",
            _acquisition.nss(),
            [&secondaryIndexSpecs, this] {
                CollectionWriter collWriter(_opCtx.get(), &_acquisition);
                WriteUnitOfWork wuow(_opCtx.get());
                // All writes in CollectionBulkLoaderImpl should be unreplicated.
                // The opCtx is accessed indirectly through _secondaryIndexesBlock.
                UnreplicatedWritesBlock uwb(_opCtx.get());
                // This enforces the buildIndexes setting in the replica set configuration.
                auto indexCatalog =
                    collWriter.getWritableCollection(_opCtx.get())->getIndexCatalog();
                auto specs = indexCatalog->removeExistingIndexesNoChecks(
                    _opCtx.get(), collWriter.get(), secondaryIndexSpecs);
                if (specs.size()) {
                    _secondaryIndexesBlock->ignoreUniqueConstraint();
                    auto status = _secondaryIndexesBlock
                                      ->init(_opCtx.get(),
                                             collWriter,
                                             specs,
                                             MultiIndexBlock::kNoopOnInitFn,
                                             MultiIndexBlock::InitMode::InitialSync)
                                      .getStatus();
                    if (!status.isOK()) {
                        return status;
                    }
                } else {
                    _secondaryIndexesBlock.reset();
                }
                if (!_idIndexSpec.isEmpty()) {
                    auto status = _idIndexBlock
                                      ->init(_opCtx.get(),
                                             collWriter,
                                             _idIndexSpec,
                                             MultiIndexBlock::kNoopOnInitFn)
                                      .getStatus();
                    if (!status.isOK()) {
                        return status;
                    }
                } else {
                    _idIndexBlock.reset();
                }

                wuow.commit();
                return Status::OK();
            });
    });
}

Status CollectionBulkLoaderImpl::_insertDocumentsForUncappedCollection(
    const std::vector<BSONObj>::const_iterator begin,
    const std::vector<BSONObj>::const_iterator end) {
    auto iter = begin;
    while (iter != end) {
        std::vector<RecordId> locs;
        Status status = writeConflictRetry(
            _opCtx.get(), "CollectionBulkLoaderImpl/insertDocumentsUncapped", _nss, [&] {
                WriteUnitOfWork wunit(_opCtx.get());
                auto insertIter = iter;
                int bytesInBlock = 0;
                locs.clear();

                auto onRecordInserted = [&](const RecordId& location) {
                    locs.emplace_back(location);
                    return Status::OK();
                };

                while (insertIter != end && bytesInBlock < collectionBulkLoaderBatchSizeInBytes) {
                    const auto& doc = *insertIter++;
                    bytesInBlock += doc.objsize();
                    // This version of insert will not update any indexes.
                    auto status = collection_internal::insertDocumentForBulkLoader(
                        _opCtx.get(), _acquisition.getCollectionPtr(), doc, onRecordInserted);
                    if (!status.isOK()) {
                        return status;
                    }
                }

                wunit.commit();
                return Status::OK();
            });

        if (!status.isOK()) {
            return status;
        }

        // Inserts index entries into the external sorter. This will not update pre-existing
        // indexes. Wrap this in a WUOW since the index entry insertion may modify the durable
        // record store which can throw a write conflict exception.
        status = writeConflictRetry(_opCtx.get(), "_addDocumentToIndexBlocks", _nss, [&] {
            WriteUnitOfWork wunit(_opCtx.get());
            for (size_t index = 0; index < locs.size(); ++index) {
                status = _addDocumentToIndexBlocks(*iter++, locs.at(index));
                if (!status.isOK()) {
                    return status;
                }
            }
            wunit.commit();
            return Status::OK();
        });

        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status CollectionBulkLoaderImpl::_insertDocumentsForCappedCollection(
    const std::vector<BSONObj>::const_iterator begin,
    const std::vector<BSONObj>::const_iterator end) {
    for (auto iter = begin; iter != end; ++iter) {
        const auto& doc = *iter;
        Status status = writeConflictRetry(
            _opCtx.get(), "CollectionBulkLoaderImpl/insertDocumentsCapped", _nss, [&] {
                WriteUnitOfWork wunit(_opCtx.get());
                // For capped collections, we use regular insertDocument, which
                // will update pre-existing indexes.
                auto status = collection_internal::insertDocument(
                    _opCtx.get(), _acquisition.getCollectionPtr(), InsertStatement(doc), nullptr);
                if (!status.isOK()) {
                    return status;
                }
                wunit.commit();
                return Status::OK();
            });
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

Status CollectionBulkLoaderImpl::insertDocuments(const std::vector<BSONObj>::const_iterator begin,
                                                 const std::vector<BSONObj>::const_iterator end) {
    return _runTaskReleaseResourcesOnFailure([&] {
        UnreplicatedWritesBlock uwb(_opCtx.get());
        if (_idIndexBlock || _secondaryIndexesBlock) {
            return _insertDocumentsForUncappedCollection(begin, end);
        } else {
            return _insertDocumentsForCappedCollection(begin, end);
        }
    });
}

Status CollectionBulkLoaderImpl::commit() {
    return _runTaskReleaseResourcesOnFailure([&] {
        _stats.startBuildingIndexes = Date_t::now();
        LOGV2_DEBUG(
            21130, 2, "Creating indexes for ns: {namespace}", "Creating indexes", logAttrs(_nss));
        UnreplicatedWritesBlock uwb(_opCtx.get());

        // Commit before deleting dups, so the dups will be removed from secondary indexes when
        // deleted.
        if (_secondaryIndexesBlock) {
            auto status = _secondaryIndexesBlock->dumpInsertsFromBulk(
                _opCtx.get(), _acquisition.getCollectionPtr());
            if (!status.isOK()) {
                return status;
            }

            // This should always return Status::OK() as the foreground index build doesn't install
            // an interceptor.
            invariant(_secondaryIndexesBlock->checkConstraints(_opCtx.get(),
                                                               _acquisition.getCollectionPtr()));

            status =
                writeConflictRetry(_opCtx.get(), "CollectionBulkLoaderImpl::commit", _nss, [this] {
                    CollectionWriter collWriter(_opCtx.get(), &_acquisition);
                    WriteUnitOfWork wunit(_opCtx.get());
                    auto status = _secondaryIndexesBlock->commit(
                        _opCtx.get(),
                        collWriter.getWritableCollection(_opCtx.get()),
                        MultiIndexBlock::kNoopOnCreateEachFn,
                        MultiIndexBlock::kNoopOnCommitFn);
                    if (!status.isOK()) {
                        return status;
                    }
                    wunit.commit();
                    return Status::OK();
                });
            if (!status.isOK()) {
                return status;
            }
        }

        if (_idIndexBlock) {
            // Do not do inside a WriteUnitOfWork (required by dumpInsertsFromBulk).
            auto status = _idIndexBlock->dumpInsertsFromBulk(
                _opCtx.get(), _acquisition.getCollectionPtr(), [&](const RecordId& rid) {
                    writeConflictRetry(
                        _opCtx.get(), "CollectionBulkLoaderImpl::commit", _nss, [this, &rid] {
                            WriteUnitOfWork wunit(_opCtx.get());

                            auto doc = _acquisition.getCollectionPtr()->docFor(_opCtx.get(), rid);

                            // Delete the document before committing the index. If we were to delete
                            // the document after committing the index, it's possible that the we
                            // may unindex a record with the same key but a different RecordId.
                            _acquisition.getCollectionPtr()->getRecordStore()->deleteRecord(
                                _opCtx.get(), rid);

                            auto indexIt =
                                _acquisition.getCollectionPtr()
                                    ->getIndexCatalog()
                                    ->getIndexIterator(_opCtx.get(),
                                                       IndexCatalog::InclusionPolicy::kReady);
                            while (auto entry = indexIt->next()) {
                                if (entry->descriptor()->isIdIndex()) {
                                    continue;
                                }

                                SharedBufferFragmentBuilder pooledBuilder{
                                    key_string::HeapBuilder::kHeapAllocatorDefaultBytes};

                                InsertDeleteOptions options;
                                options.dupsAllowed = !entry->descriptor()->unique();

                                entry->accessMethod()->remove(
                                    _opCtx.get(),
                                    pooledBuilder,
                                    _acquisition.getCollectionPtr(),
                                    entry,
                                    doc.value(),
                                    rid,
                                    false /* logIfError */,
                                    options,
                                    nullptr /* numDeleted */,
                                    // Initial sync can build an index over a collection with
                                    // duplicates, so we need to check the RecordId of the docuemnt
                                    // we are unindexing. See SERVER-17487 for more details.
                                    CheckRecordId::On);
                            }

                            wunit.commit();
                        });
                    return Status::OK();
                });
            if (!status.isOK()) {
                return status;
            }

            // Commit the _id index, there won't be any documents with duplicate _ids as they were
            // deleted prior to this.
            status =
                writeConflictRetry(_opCtx.get(), "CollectionBulkLoaderImpl::commit", _nss, [this] {
                    CollectionWriter collWriter(_opCtx.get(), &_acquisition);
                    WriteUnitOfWork wunit(_opCtx.get());
                    auto status =
                        _idIndexBlock->commit(_opCtx.get(),
                                              collWriter.getWritableCollection(_opCtx.get()),
                                              MultiIndexBlock::kNoopOnCreateEachFn,
                                              MultiIndexBlock::kNoopOnCommitFn);
                    if (!status.isOK()) {
                        return status;
                    }
                    wunit.commit();
                    return Status::OK();
                });
            if (!status.isOK()) {
                return status;
            }
        }

        _stats.endBuildingIndexes = Date_t::now();
        LOGV2_DEBUG(21131,
                    2,
                    "Done creating indexes for ns: {namespace}, stats: {stats}",
                    "Done creating indexes",
                    logAttrs(_nss),
                    "stats"_attr = _stats.toString());

        // Clean up here so we do not try to abort the index builds when cleaning up in
        // _releaseResources.
        _idIndexBlock.reset();
        _secondaryIndexesBlock.reset();
        return Status::OK();
    });
}

void CollectionBulkLoaderImpl::_releaseResources() {
    invariant(&cc() == _opCtx->getClient());
    if (_secondaryIndexesBlock) {
        CollectionWriter collWriter(_opCtx.get(), &_acquisition);
        _secondaryIndexesBlock->abortIndexBuild(
            _opCtx.get(), collWriter, MultiIndexBlock::kNoopOnCleanUpFn);
        _secondaryIndexesBlock.reset();
    }

    if (_idIndexBlock) {
        CollectionWriter collWriter(_opCtx.get(), &_acquisition);
        _idIndexBlock->abortIndexBuild(_opCtx.get(), collWriter, MultiIndexBlock::kNoopOnCleanUpFn);
        _idIndexBlock.reset();
    }

    // Locks will get released when we destroy the object.
}

template <typename F>
Status CollectionBulkLoaderImpl::_runTaskReleaseResourcesOnFailure(const F& task) noexcept {
    AlternativeClientRegion acr(_client);
    ScopeGuard guard([this] { _releaseResources(); });
    try {
        auto status = task();
        if (status.isOK()) {
            guard.dismiss();
        }
        return status;
    } catch (...) {
        std::terminate();
    }
}

Status CollectionBulkLoaderImpl::_addDocumentToIndexBlocks(const BSONObj& doc,
                                                           const RecordId& loc) {
    if (_idIndexBlock) {
        auto status = _idIndexBlock->insertSingleDocumentForInitialSyncOrRecovery(
            _opCtx.get(),
            _acquisition.getCollectionPtr(),
            doc,
            loc,
            // This caller / code path does not have cursors to save/restore.
            /*saveCursorBeforeWrite*/ []() {},
            /*restoreCursorAfterWrite*/ []() {});
        if (!status.isOK()) {
            return status.withContext("failed to add document to _id index");
        }
    }

    if (_secondaryIndexesBlock) {
        auto status = _secondaryIndexesBlock->insertSingleDocumentForInitialSyncOrRecovery(
            _opCtx.get(),
            _acquisition.getCollectionPtr(),
            doc,
            loc,
            // This caller / code path does not have cursors to save/restore.
            /*saveCursorBeforeWrite*/ []() {},
            /*restoreCursorAfterWrite*/ []() {});
        if (!status.isOK()) {
            return status.withContext("failed to add document to secondary indexes");
        }
    }

    return Status::OK();
}

CollectionBulkLoaderImpl::Stats CollectionBulkLoaderImpl::getStats() const {
    return _stats;
}

std::string CollectionBulkLoaderImpl::Stats::toString() const {
    return toBSON().toString();
}

BSONObj CollectionBulkLoaderImpl::Stats::toBSON() const {
    BSONObjBuilder bob;
    bob.appendDate("startBuildingIndexes", startBuildingIndexes);
    bob.appendDate("endBuildingIndexes", endBuildingIndexes);
    auto indexElapsed = endBuildingIndexes - startBuildingIndexes;
    long long indexElapsedMillis = duration_cast<Milliseconds>(indexElapsed).count();
    bob.appendNumber("indexElapsedMillis", indexElapsedMillis);
    return bob.obj();
}

}  // namespace repl
}  // namespace mongo
