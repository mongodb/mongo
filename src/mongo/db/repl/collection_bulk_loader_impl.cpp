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

namespace mongo {
namespace repl {

CollectionBulkLoaderImpl::CollectionBulkLoaderImpl(OperationContext* txn,
                                                   TaskRunner* runner,
                                                   Collection* coll,
                                                   const BSONObj idIndexSpec,
                                                   std::unique_ptr<AutoGetOrCreateDb> autoDb,
                                                   std::unique_ptr<AutoGetCollection> autoColl)
    : _runner(runner),
      _autoColl(std::move(autoColl)),
      _autoDB(std::move(autoDb)),
      _txn(txn),
      _coll(coll),
      _nss{coll->ns()},
      _idIndexBlock{txn, coll},
      _secondaryIndexesBlock{txn, coll},
      _idIndexSpec(idIndexSpec) {
    invariant(txn);
    invariant(coll);
    invariant(runner);
    invariant(_autoDB);
    invariant(_autoColl);
    invariant(_autoDB->getDb());
    invariant(_autoColl->getDb() == _autoDB->getDb());
}

CollectionBulkLoaderImpl::~CollectionBulkLoaderImpl() {
    DESTRUCTOR_GUARD(_runner->cancel();)
}

Status CollectionBulkLoaderImpl::init(OperationContext* txn,
                                      Collection* coll,
                                      const std::vector<BSONObj>& secondaryIndexSpecs) {
    invariant(txn);
    invariant(coll);
    invariant(txn->getClient() == &cc());
    if (secondaryIndexSpecs.size()) {
        _hasSecondaryIndexes = true;
        _secondaryIndexesBlock.ignoreUniqueConstraint();
        auto status = _secondaryIndexesBlock.init(secondaryIndexSpecs);
        if (!status.isOK()) {
            return status;
        }
    }
    if (!_idIndexSpec.isEmpty()) {
        auto status = _idIndexBlock.init(_idIndexSpec);
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status CollectionBulkLoaderImpl::insertDocuments(const std::vector<BSONObj>::const_iterator begin,
                                                 const std::vector<BSONObj>::const_iterator end) {
    int count = 0;
    return _runner->runSynchronousTask([begin, end, &count, this](OperationContext* txn) -> Status {
        invariant(txn);

        for (auto iter = begin; iter != end; ++iter) {
            std::vector<MultiIndexBlock*> indexers;
            if (!_idIndexSpec.isEmpty()) {
                indexers.push_back(&_idIndexBlock);
            }
            if (_hasSecondaryIndexes) {
                indexers.push_back(&_secondaryIndexesBlock);
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
    return _runner->runSynchronousTask(
        [this](OperationContext* txn) -> Status {
            invariant(txn->getClient() == &cc());
            invariant(txn == _txn);

            // Commit before deleting dups, so the dups will be removed from secondary indexes when
            // deleted.
            if (_hasSecondaryIndexes) {
                std::set<RecordId> secDups;
                auto status = _secondaryIndexesBlock.doneInserting(&secDups);
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
                    _secondaryIndexesBlock.commit();
                    wunit.commit();
                }
                MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
                    _txn, "CollectionBulkLoaderImpl::commit", _nss.ns());
            }

            if (!_idIndexSpec.isEmpty()) {
                // Delete dups.
                std::set<RecordId> dups;
                // Do not do inside a WriteUnitOfWork (required by doneInserting).
                auto status = _idIndexBlock.doneInserting(&dups);
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
                    _idIndexBlock.commit();
                    wunit.commit();
                }
                MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
                    _txn, "CollectionBulkLoaderImpl::commit", _nss.ns());
            }

            // release locks.
            _autoColl.reset(nullptr);
            _autoDB.reset(nullptr);
            _coll = nullptr;
            return Status::OK();
        },
        TaskRunner::NextAction::kDisposeOperationContext);
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
