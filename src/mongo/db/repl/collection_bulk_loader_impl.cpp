
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
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

CollectionBulkLoaderImpl::CollectionBulkLoaderImpl(ServiceContext::UniqueClient&& client,
                                                   ServiceContext::UniqueOperationContext&& opCtx,
                                                   std::unique_ptr<AutoGetCollection>&& autoColl,
                                                   const BSONObj& idIndexSpec)
    : _client{std::move(client)},
      _opCtx{std::move(opCtx)},
      _autoColl{std::move(autoColl)},
      _nss{_autoColl->getCollection()->ns()},
      _idIndexBlock(std::make_unique<MultiIndexBlock>(_opCtx.get(), _autoColl->getCollection())),
      _secondaryIndexesBlock(
          std::make_unique<MultiIndexBlock>(_opCtx.get(), _autoColl->getCollection())),
      _idIndexSpec(idIndexSpec.getOwned()) {

    invariant(_opCtx);
    invariant(_autoColl->getCollection());
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
            auto specs = indexCatalog->removeExistingIndexes(_opCtx.get(), secondaryIndexSpecs);
            if (specs.size()) {
                _secondaryIndexesBlock->ignoreUniqueConstraint();
                auto status =
                    _secondaryIndexesBlock->init(specs, MultiIndexBlock::kNoopOnInitFn).getStatus();
                if (!status.isOK()) {
                    return status;
                }
            } else {
                _secondaryIndexesBlock.reset();
            }
            if (!_idIndexSpec.isEmpty()) {
                auto status =
                    _idIndexBlock->init(_idIndexSpec, MultiIndexBlock::kNoopOnInitFn).getStatus();
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
    int count = 0;
    return _runTaskReleaseResourcesOnFailure([&] {
        UnreplicatedWritesBlock uwb(_opCtx.get());

        for (auto iter = begin; iter != end; ++iter) {
            Status status = writeConflictRetry(
                _opCtx.get(), "CollectionBulkLoaderImpl::insertDocuments", _nss.ns(), [&] {
                    WriteUnitOfWork wunit(_opCtx.get());
                    const auto& doc = *iter;
                    if (_idIndexBlock || _secondaryIndexesBlock) {
                        // This flavor of insertDocument will not update any pre-existing indexes,
                        // only the indexers passed in.
                        auto onRecordInserted = [&](const RecordId& loc) {
                            return _addDocumentToIndexBlocks(doc, loc);
                        };
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

            ++count;
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
            std::set<RecordId> secDups;
            auto status = _secondaryIndexesBlock->dumpInsertsFromBulk(&secDups);
            if (!status.isOK()) {
                return status;
            }
            if (secDups.size()) {
                return Status{ErrorCodes::UserDataInconsistent,
                              str::stream() << "Found " << secDups.size()
                                            << " duplicates on secondary index(es) even though "
                                               "MultiIndexBlock::ignoreUniqueConstraint set."};
            }
            status = writeConflictRetry(
                _opCtx.get(), "CollectionBulkLoaderImpl::commit", _nss.ns(), [this] {
                    WriteUnitOfWork wunit(_opCtx.get());
                    auto status = _secondaryIndexesBlock->commit(
                        MultiIndexBlock::kNoopOnCreateEachFn, MultiIndexBlock::kNoopOnCommitFn);
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
            auto status = _idIndexBlock->dumpInsertsFromBulk(&dups);
            if (!status.isOK()) {
                return status;
            }

            // Commit _id index without duplicate keys even though there may still be documents
            // with duplicate _ids. These duplicates will be deleted in the following step.
            status = writeConflictRetry(
                _opCtx.get(), "CollectionBulkLoaderImpl::commit", _nss.ns(), [this] {
                    WriteUnitOfWork wunit(_opCtx.get());
                    auto status = _idIndexBlock->commit(MultiIndexBlock::kNoopOnCreateEachFn,
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

            // Delete duplicate records after committing the index so these writes are not
            // intercepted by the still in-progress index builder.
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
        }
        _stats.endBuildingIndexes = Date_t::now();
        LOG(2) << "Done creating indexes for ns: " << _nss.ns() << ", stats: " << _stats.toString();

        _releaseResources();
        return Status::OK();
    });
}

void CollectionBulkLoaderImpl::_releaseResources() {
    invariant(&cc() == _opCtx->getClient());
    if (_secondaryIndexesBlock)
        _secondaryIndexesBlock.reset();

    if (_idIndexBlock)
        _idIndexBlock.reset();

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
        auto status = _idIndexBlock->insert(doc, loc);
        if (!status.isOK()) {
            return status.withContext("failed to add document to _id index");
        }
    }

    if (_secondaryIndexesBlock) {
        auto status = _secondaryIndexesBlock->insert(doc, loc);
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
