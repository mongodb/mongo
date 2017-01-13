/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_create.h"
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

CollectionBulkLoaderImpl::CollectionBulkLoaderImpl(OperationContext* txn,
                                                   Collection* coll,
                                                   const BSONObj idIndexSpec,
                                                   std::unique_ptr<OldThreadPool> threadPool,
                                                   std::unique_ptr<TaskRunner> runner,
                                                   std::unique_ptr<AutoGetOrCreateDb> autoDb,
                                                   std::unique_ptr<AutoGetCollection> autoColl)
    : _threadPool(std::move(threadPool)),
      _runner(std::move(runner)),
      _autoColl(std::move(autoColl)),
      _autoDB(std::move(autoDb)),
      _txn(txn),
      _coll(coll),
      _nss{coll->ns()},
      _idIndexBlock(stdx::make_unique<MultiIndexBlock>(txn, coll)),
      _secondaryIndexesBlock(stdx::make_unique<MultiIndexBlock>(txn, coll)),
      _idIndexSpec(idIndexSpec) {
    invariant(txn);
    invariant(coll);
    invariant(_runner);
    invariant(_autoDB);
    invariant(_autoColl);
    invariant(_autoDB->getDb());
    invariant(_autoColl->getDb() == _autoDB->getDb());
}

CollectionBulkLoaderImpl::~CollectionBulkLoaderImpl() {
    DESTRUCTOR_GUARD({
        _releaseResources();
        _runner->cancel();
        _runner->join();
        _threadPool->join();
    })
}

Status CollectionBulkLoaderImpl::init(Collection* coll,
                                      const std::vector<BSONObj>& secondaryIndexSpecs) {
    return _runTaskReleaseResourcesOnFailure(
        [coll, &secondaryIndexSpecs, this](OperationContext* txn) -> Status {
            invariant(txn);
            invariant(coll);
            invariant(txn->getClient() == &cc());
            std::vector<BSONObj> specs(secondaryIndexSpecs);
            // This enforces the buildIndexes setting in the replica set configuration.
            _secondaryIndexesBlock->removeExistingIndexes(&specs);
            if (specs.size()) {
                _secondaryIndexesBlock->ignoreUniqueConstraint();
                auto status = _secondaryIndexesBlock->init(specs).getStatus();
                if (!status.isOK()) {
                    return status;
                }
            } else {
                _secondaryIndexesBlock.reset();
            }
            if (!_idIndexSpec.isEmpty()) {
                auto status = _idIndexBlock->init(_idIndexSpec).getStatus();
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
    return _runTaskReleaseResourcesOnFailure(
        [begin, end, &count, this](OperationContext* txn) -> Status {
            invariant(txn);

            for (auto iter = begin; iter != end; ++iter) {
                std::vector<MultiIndexBlock*> indexers;
                if (_idIndexBlock) {
                    indexers.push_back(_idIndexBlock.get());
                }
                if (_secondaryIndexesBlock) {
                    indexers.push_back(_secondaryIndexesBlock.get());
                }
                MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                    WriteUnitOfWork wunit(txn);
                    const auto status = _coll->insertDocument(txn, *iter, indexers, false);
                    if (!status.isOK()) {
                        return status;
                    }
                    wunit.commit();
                }
                MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
                    _txn, "CollectionBulkLoaderImpl::insertDocuments", _nss.ns());

                ++count;
            }
            return Status::OK();
        });
}

Status CollectionBulkLoaderImpl::commit() {
    return _runTaskReleaseResourcesOnFailure(
        [this](OperationContext* txn) -> Status {
            _stats.startBuildingIndexes = Date_t::now();
            LOG(2) << "Creating indexes for ns: " << _nss.ns();
            invariant(txn->getClient() == &cc());
            invariant(txn == _txn);

            // Commit before deleting dups, so the dups will be removed from secondary indexes when
            // deleted.
            if (_secondaryIndexesBlock) {
                std::set<RecordId> secDups;
                auto status = _secondaryIndexesBlock->doneInserting(&secDups);
                if (!status.isOK()) {
                    return status;
                }
                if (secDups.size()) {
                    return Status{ErrorCodes::UserDataInconsistent,
                                  str::stream() << "Found " << secDups.size()
                                                << " duplicates on secondary index(es) even though "
                                                   "MultiIndexBlock::ignoreUniqueConstraint set."};
                }
                MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                    WriteUnitOfWork wunit(txn);
                    _secondaryIndexesBlock->commit();
                    wunit.commit();
                }
                MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
                    _txn, "CollectionBulkLoaderImpl::commit", _nss.ns());
            }

            if (_idIndexBlock) {
                // Delete dups.
                std::set<RecordId> dups;
                // Do not do inside a WriteUnitOfWork (required by doneInserting).
                auto status = _idIndexBlock->doneInserting(&dups);
                if (!status.isOK()) {
                    return status;
                }

                for (auto&& it : dups) {
                    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                        WriteUnitOfWork wunit(_txn);
                        _coll->deleteDocument(_txn,
                                              it,
                                              nullptr /** OpDebug **/,
                                              false /* fromMigrate */,
                                              true /* noWarn */);
                        wunit.commit();
                    }
                    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
                        _txn, "CollectionBulkLoaderImpl::commit", _nss.ns());
                }

                // Commit _id index, without dups.
                MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                    WriteUnitOfWork wunit(txn);
                    _idIndexBlock->commit();
                    wunit.commit();
                }
                MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
                    _txn, "CollectionBulkLoaderImpl::commit", _nss.ns());
            }
            _stats.endBuildingIndexes = Date_t::now();
            LOG(2) << "Done creating indexes for ns: " << _nss.ns()
                   << ", stats: " << _stats.toString();

            _releaseResources();
            return Status::OK();
        },
        TaskRunner::NextAction::kDisposeOperationContext);
}

void CollectionBulkLoaderImpl::_releaseResources() {
    if (_secondaryIndexesBlock) {
        // A valid Client is required to drop unfinished indexes.
        Client::initThreadIfNotAlready();
        _secondaryIndexesBlock.reset();
    }

    if (_idIndexBlock) {
        // A valid Client is required to drop unfinished indexes.
        Client::initThreadIfNotAlready();
        _idIndexBlock.reset();
    }

    // release locks.
    _coll = nullptr;
    _autoColl.reset(nullptr);
    _autoDB.reset(nullptr);
}

Status CollectionBulkLoaderImpl::_runTaskReleaseResourcesOnFailure(
    TaskRunner::SynchronousTask task, TaskRunner::NextAction nextAction) {
    auto newTask = [this, &task](OperationContext* txn) -> Status {
        ScopeGuard guard = MakeGuard(&CollectionBulkLoaderImpl::_releaseResources, this);
        const auto status = task(txn);
        if (status.isOK()) {
            guard.Dismiss();
        }
        return status;
    };
    return _runner->runSynchronousTask(newTask, nextAction);
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
