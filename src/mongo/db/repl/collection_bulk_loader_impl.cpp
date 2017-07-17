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

namespace {

/**
 * Utility class to temporarily swap which client is bound to the running thread.
 *
 * Use this class to bind a client to the current thread for the duration of the
 * AlternativeClientRegion's lifetime, restoring the prior client, if any, at the
 * end of the block.
 */
class AlternativeClientRegion {
public:
    explicit AlternativeClientRegion(ServiceContext::UniqueClient& clientToUse)
        : _alternateClient(&clientToUse) {
        invariant(clientToUse);
        if (Client::getCurrent()) {
            _originalClient = Client::releaseCurrent();
        }
        Client::setCurrent(std::move(*_alternateClient));
    }

    ~AlternativeClientRegion() {
        *_alternateClient = Client::releaseCurrent();
        if (_originalClient) {
            Client::setCurrent(std::move(_originalClient));
        }
    }

private:
    ServiceContext::UniqueClient _originalClient;
    ServiceContext::UniqueClient* const _alternateClient;
};

}  // namespace

CollectionBulkLoaderImpl::CollectionBulkLoaderImpl(ServiceContext::UniqueClient&& client,
                                                   ServiceContext::UniqueOperationContext&& opCtx,
                                                   std::unique_ptr<AutoGetCollection>&& autoColl,
                                                   const BSONObj& idIndexSpec)
    : _client{std::move(client)},
      _opCtx{std::move(opCtx)},
      _autoColl{std::move(autoColl)},
      _nss{_autoColl->getCollection()->ns()},
      _idIndexBlock(stdx::make_unique<MultiIndexBlock>(_opCtx.get(), _autoColl->getCollection())),
      _secondaryIndexesBlock(
          stdx::make_unique<MultiIndexBlock>(_opCtx.get(), _autoColl->getCollection())),
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
    return _runTaskReleaseResourcesOnFailure([&]() -> Status {
        UnreplicatedWritesBlock uwb(_opCtx.get());

        for (auto iter = begin; iter != end; ++iter) {
            std::vector<MultiIndexBlock*> indexers;
            if (_idIndexBlock) {
                indexers.push_back(_idIndexBlock.get());
            }
            if (_secondaryIndexesBlock) {
                indexers.push_back(_secondaryIndexesBlock.get());
            }

            Status status = writeConflictRetry(
                _opCtx.get(), "CollectionBulkLoaderImpl::insertDocuments", _nss.ns(), [&] {
                    WriteUnitOfWork wunit(_opCtx.get());
                    if (!indexers.empty()) {
                        // This flavor of insertDocument will not update any pre-existing indexes,
                        // only the indexers passed in.
                        const auto status = _autoColl->getCollection()->insertDocument(
                            _opCtx.get(), *iter, indexers, false);
                        if (!status.isOK()) {
                            return status;
                        }
                    } else {
                        // For capped collections, we use regular insertDocument, which will update
                        // pre-existing indexes.
                        const auto status = _autoColl->getCollection()->insertDocument(
                            _opCtx.get(), InsertStatement(*iter), nullptr, false);
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
    return _runTaskReleaseResourcesOnFailure([this]() -> Status {
        _stats.startBuildingIndexes = Date_t::now();
        LOG(2) << "Creating indexes for ns: " << _nss.ns();
        UnreplicatedWritesBlock uwb(_opCtx.get());

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
            writeConflictRetry(_opCtx.get(), "CollectionBulkLoaderImpl::commit", _nss.ns(), [this] {
                WriteUnitOfWork wunit(_opCtx.get());
                _secondaryIndexesBlock->commit();
                wunit.commit();
            });
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

            // Commit _id index, without dups.
            writeConflictRetry(_opCtx.get(), "CollectionBulkLoaderImpl::commit", _nss.ns(), [this] {
                WriteUnitOfWork wunit(_opCtx.get());
                _idIndexBlock->commit();
                wunit.commit();
            });
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
    _autoColl.reset();
}

template <typename F>
Status CollectionBulkLoaderImpl::_runTaskReleaseResourcesOnFailure(F task) noexcept {

    AlternativeClientRegion acr(_client);
    ScopeGuard guard = MakeGuard(&CollectionBulkLoaderImpl::_releaseResources, this);
    try {
        const auto status = [&task]() noexcept {
            return task();
        }
        ();
        if (status.isOK()) {
            guard.Dismiss();
        }
        return status;
    } catch (...) {
        std::terminate();
    }
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
