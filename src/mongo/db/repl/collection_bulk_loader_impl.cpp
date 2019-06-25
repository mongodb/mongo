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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/collection_bulk_loader_impl.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {
namespace repl {

CollectionBulkLoaderImpl::CollectionBulkLoaderImpl(ServiceContext::UniqueClient&& client,
                                                   ServiceContext::UniqueOperationContext&& opCtx,
                                                   std::unique_ptr<AutoGetCollection>&& autoColl,
                                                   const BSONObj& idIndexSpec)
    : _client{std::move(client)},
      _opCtx{std::move(opCtx)},
      _autoColl{std::move(autoColl)},
      _collection{_autoColl->getCollection()},
      _nss{_autoColl->getCollection()->ns()},
      _idIndexBlock(std::make_unique<MultiIndexBlock>()),
      _secondaryIndexesBlock(std::make_unique<MultiIndexBlock>()),
      _idIndexSpec(idIndexSpec.getOwned()) {

    invariant(_opCtx);
    invariant(_collection);
}

CollectionBulkLoaderImpl::~CollectionBulkLoaderImpl() {
    AlternativeClientRegion acr(_client);
    DESTRUCTOR_GUARD({ _releaseResources(); })
}

Status CollectionBulkLoaderImpl::init(const std::vector<BSONObj>& secondaryIndexSpecs) {
    return _runTaskReleaseResourcesOnFailure(
        [ coll = _autoColl->getCollection(), &secondaryIndexSpecs, this ]()->Status {
            // All writes in CollectionBulkLoaderImpl should be unreplicated.
            // The opCtx is accessed indirectly through _secondaryIndexesBlock.
            UnreplicatedWritesBlock uwb(_opCtx.get());
            // This enforces the buildIndexes setting in the replica set configuration.
            auto indexCatalog = coll->getIndexCatalog();
            auto specs =
                indexCatalog->removeExistingIndexesNoChecks(_opCtx.get(), secondaryIndexSpecs);
            if (specs.size()) {
                _secondaryIndexesBlock->ignoreUniqueConstraint();
                auto status =
                    _secondaryIndexesBlock
                        ->init(_opCtx.get(), _collection, specs, MultiIndexBlock::kNoopOnInitFn)
                        .getStatus();
                if (!status.isOK()) {
                    return status;
                }
            } else {
                _secondaryIndexesBlock.reset();
            }
            if (!_idIndexSpec.isEmpty()) {
                auto status =
                    _idIndexBlock
                        ->init(
                            _opCtx.get(), _collection, _idIndexSpec, MultiIndexBlock::kNoopOnInitFn)
                        .getStatus();
                if (!status.isOK()) {
                    return status;
                }
            } else {
                _idIndexBlock.reset();
            }

            return Status::OK();
        });
}

Status CollectionBulkLoaderImpl::insertDocuments(const std::vector<BSONObj>::const_iterator begin,
                                                 const std::vector<BSONObj>::const_iterator end) {
    return _runTaskReleaseResourcesOnFailure([&] {
        UnreplicatedWritesBlock uwb(_opCtx.get());

        for (auto iter = begin; iter != end; ++iter) {
            boost::optional<RecordId> loc;
            const auto& doc = *iter;
            Status status = writeConflictRetry(
                _opCtx.get(), "CollectionBulkLoaderImpl::insertDocuments", _nss.ns(), [&] {
                    WriteUnitOfWork wunit(_opCtx.get());
                    if (_idIndexBlock || _secondaryIndexesBlock) {
                        auto onRecordInserted = [&](const RecordId& location) {
                            loc = location;
                            return Status::OK();
                        };
                        // This version of insert will not update any indexes.
                        const auto status = _autoColl->getCollection()->insertDocumentForBulkLoader(
                            _opCtx.get(), doc, onRecordInserted);
                        if (!status.isOK()) {
                            return status;
                        }
                    } else {
                        // For capped collections, we use regular insertDocument, which will update
                        // pre-existing indexes.
                        const auto status = _autoColl->getCollection()->insertDocument(
                            _opCtx.get(), InsertStatement(doc), nullptr);
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

            if (loc) {
                // Inserts index entries into the external sorter. This will not update
                // pre-existing indexes.
                status = _addDocumentToIndexBlocks(doc, loc.get());
            }

            if (!status.isOK()) {
                return status;
            }
        }
        return Status::OK();
    });
}

Status CollectionBulkLoaderImpl::commit() {
    return _runTaskReleaseResourcesOnFailure([&] {
        _stats.startBuildingIndexes = Date_t::now();
        LOG(2) << "Creating indexes for ns: " << _nss.ns();
        UnreplicatedWritesBlock uwb(_opCtx.get());

        // Commit before deleting dups, so the dups will be removed from secondary indexes when
        // deleted.
        if (_secondaryIndexesBlock) {
            auto status = _secondaryIndexesBlock->dumpInsertsFromBulk(_opCtx.get());
            if (!status.isOK()) {
                return status;
            }

            // This should always return Status::OK() as secondary index builds ignore duplicate key
            // constraints causing them to not be recorded.
            invariant(_secondaryIndexesBlock->checkConstraints(_opCtx.get()));

            status = writeConflictRetry(
                _opCtx.get(), "CollectionBulkLoaderImpl::commit", _nss.ns(), [this] {
                    WriteUnitOfWork wunit(_opCtx.get());
                    auto status =
                        _secondaryIndexesBlock->commit(_opCtx.get(),
                                                       _collection,
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
            // Gather RecordIds for uninserted duplicate keys to delete.
            std::set<RecordId> dups;
            // Do not do inside a WriteUnitOfWork (required by dumpInsertsFromBulk).
            auto status = _idIndexBlock->dumpInsertsFromBulk(_opCtx.get(), &dups);
            if (!status.isOK()) {
                return status;
            }

            // If we were to delete the documents after committing the index build, it's possible
            // that the storage engine unindexes a different record with the same key, but different
            // RecordId. By deleting documents before committing the index build, the index removal
            // code uses 'dupsAllowed', which forces the storage engine to only unindex records that
            // match the same key and RecordId.
            for (auto&& it : dups) {
                writeConflictRetry(
                    _opCtx.get(), "CollectionBulkLoaderImpl::commit", _nss.ns(), [this, &it] {
                        WriteUnitOfWork wunit(_opCtx.get());
                        _autoColl->getCollection()->deleteDocument(_opCtx.get(),
                                                                   kUninitializedStmtId,
                                                                   it,
                                                                   nullptr /** OpDebug **/,
                                                                   false /* fromMigrate */,
                                                                   true /* noWarn */);
                        wunit.commit();
                    });
            }

            status = _idIndexBlock->drainBackgroundWrites(_opCtx.get());
            if (!status.isOK()) {
                return status;
            }

            status = _idIndexBlock->checkConstraints(_opCtx.get());
            if (!status.isOK()) {
                return status;
            }

            // Commit the _id index, there won't be any documents with duplicate _ids as they were
            // deleted prior to this.
            status = writeConflictRetry(
                _opCtx.get(), "CollectionBulkLoaderImpl::commit", _nss.ns(), [this] {
                    WriteUnitOfWork wunit(_opCtx.get());
                    auto status = _idIndexBlock->commit(_opCtx.get(),
                                                        _collection,
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
        LOG(2) << "Done creating indexes for ns: " << _nss.ns() << ", stats: " << _stats.toString();

        _releaseResources();
        return Status::OK();
    });
}

void CollectionBulkLoaderImpl::_releaseResources() {
    invariant(&cc() == _opCtx->getClient());
    if (_secondaryIndexesBlock) {
        _secondaryIndexesBlock->cleanUpAfterBuild(_opCtx.get(), _collection);
        _secondaryIndexesBlock.reset();
    }

    if (_idIndexBlock) {
        _idIndexBlock->cleanUpAfterBuild(_opCtx.get(), _collection);
        _idIndexBlock.reset();
    }

    // release locks.
    _autoColl.reset();
}

template <typename F>
Status CollectionBulkLoaderImpl::_runTaskReleaseResourcesOnFailure(const F& task) noexcept {
    AlternativeClientRegion acr(_client);
    auto guard = makeGuard([this] { _releaseResources(); });
    try {
        const auto status = task();
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
        auto status = _idIndexBlock->insert(_opCtx.get(), doc, loc);
        if (!status.isOK()) {
            return status.withContext("failed to add document to _id index");
        }
    }

    if (_secondaryIndexesBlock) {
        auto status = _secondaryIndexesBlock->insert(_opCtx.get(), doc, loc);
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


std::string CollectionBulkLoaderImpl::toString() const {
    return toBSON().toString();
}

BSONObj CollectionBulkLoaderImpl::toBSON() const {
    BSONObjBuilder bob;
    bob.append("BulkLoader", _nss.toString());
    // TODO: Add index specs here.
    return bob.done();
}

}  // namespace repl
}  // namespace mongo
