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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/index_builder.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_timestamp_helper.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/server_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::endl;

AtomicWord<unsigned> IndexBuilder::_indexBuildCount;

namespace {

const StringData kIndexesFieldName = "indexes"_sd;
const StringData kCommandName = "createIndexes"_sd;

// Synchronization tools when replication spawns a background index in a new thread.
// The bool is 'true' when a new background index has started in a new thread but the
// parent thread has not yet synchronized with it.
bool _bgIndexStarting(false);
stdx::mutex _bgIndexStartingMutex;
stdx::condition_variable _bgIndexStartingCondVar;

void _setBgIndexStarting() {
    stdx::lock_guard<stdx::mutex> lk(_bgIndexStartingMutex);
    invariant(_bgIndexStarting == false);
    _bgIndexStarting = true;
    _bgIndexStartingCondVar.notify_one();
}
}  // namespace

IndexBuilder::IndexBuilder(const BSONObj& index,
                           IndexConstraints indexConstraints,
                           ReplicatedWrites replicatedWrites,
                           Timestamp initIndexTs)
    : BackgroundJob(true /* self-delete */),
      _index(index.getOwned()),
      _indexConstraints(indexConstraints),
      _replicatedWrites(replicatedWrites),
      _initIndexTs(initIndexTs),
      _name(str::stream() << "repl-index-builder-" << _indexBuildCount.addAndFetch(1)) {}

IndexBuilder::~IndexBuilder() {}

bool IndexBuilder::canBuildInBackground() {
    return MultiIndexBlock::areHybridIndexBuildsEnabled();
}

std::string IndexBuilder::name() const {
    return _name;
}

void IndexBuilder::run() {
    ThreadClient tc(name(), getGlobalServiceContext());
    LOG(2) << "IndexBuilder building index " << _index;

    auto opCtx = cc().makeOperationContext();
    ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(opCtx->lockState());

    // If the calling thread is not replicating writes, neither should this thread.
    boost::optional<repl::UnreplicatedWritesBlock> unreplicatedWrites;
    if (_replicatedWrites == ReplicatedWrites::kUnreplicated) {
        unreplicatedWrites.emplace(opCtx.get());
    }

    AuthorizationSession::get(opCtx->getClient())->grantInternalAuthorization();

    {
        stdx::lock_guard<Client> lk(*(opCtx->getClient()));
        CurOp::get(opCtx.get())->setNetworkOp_inlock(dbInsert);
    }
    NamespaceString ns(_index["ns"].String());

    Lock::DBLock dlk(opCtx.get(), ns.db(), MODE_X);
    auto databaseHolder = DatabaseHolder::get(opCtx.get());
    auto db = databaseHolder->getDb(opCtx.get(), ns.db().toString());

    // This background index build can only be interrupted at shutdown.
    // For the duration of the OperationContext::runWithoutInterruption() invocation, any kill
    // status set by the killOp command will be ignored.
    // After OperationContext::runWithoutInterruption() returns, any call to
    // OperationContext::checkForInterrupt() will see the kill status and respond accordingly
    // (checkForInterrupt() will throw an exception while checkForInterruptNoAssert() returns
    // an error Status).
    Status status = opCtx->runWithoutInterruption([&, this] {
        return _buildAndHandleErrors(opCtx.get(), db, true /* buildInBackground */, &dlk);
    });
    if (!status.isOK()) {
        error() << "IndexBuilder could not build index: " << redact(status);
        fassert(28555, ErrorCodes::isInterruption(status.code()));
    }
}

Status IndexBuilder::buildInForeground(OperationContext* opCtx, Database* db) const {
    return _buildAndHandleErrors(opCtx, db, false /*buildInBackground */, nullptr);
}

void IndexBuilder::waitForBgIndexStarting() {
    stdx::unique_lock<stdx::mutex> lk(_bgIndexStartingMutex);
    while (_bgIndexStarting == false) {
        _bgIndexStartingCondVar.wait(lk);
    }
    // Reset for next time.
    _bgIndexStarting = false;
}

Status IndexBuilder::_buildAndHandleErrors(OperationContext* opCtx,
                                           Database* db,
                                           bool buildInBackground,
                                           Lock::DBLock* dbLock) const {
    const NamespaceString ns(_index["ns"].String());

    Collection* coll = db->getCollection(opCtx, ns);
    // Collections should not be implicitly created by the index builder.
    fassert(40409, coll);

    MultiIndexBlock indexer;

    // The 'indexer' can throw, so ensure build cleanup occurs.
    ON_BLOCK_EXIT([&] { indexer.cleanUpAfterBuild(opCtx, coll); });

    auto status = _build(opCtx, buildInBackground, coll, indexer, dbLock);
    // Background index builds are not allowed to return errors because they run in a background
    // thread.
    if (status.isOK() || !buildInBackground) {
        invariant(!dbLock || dbLock->mode() == MODE_X);
        return status;
    }

    // The MultiIndexBlock destructor may only be called when an X lock is held on the database.
    if (dbLock->mode() != MODE_X) {
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        dbLock->relockWithMode(MODE_X);
    }

    invariant(status.code() != ErrorCodes::WriteConflict);

    if (status.code() == ErrorCodes::InterruptedAtShutdown) {
        // leave it as-if kill -9 happened. This will be handled on restart.
        indexer.abortWithoutCleanup(opCtx);
        return status;
    }

    error() << "Background index build failed. Status: " << redact(status);
    fassertFailed(50769);
}

Status IndexBuilder::_build(OperationContext* opCtx,
                            bool buildInBackground,
                            Collection* coll,
                            MultiIndexBlock& indexer,
                            Lock::DBLock* dbLock) const try {
    auto ns = coll->ns();

    {
        BSONObjBuilder builder;
        builder.append(kCommandName, ns.coll());
        {
            BSONArrayBuilder indexesBuilder;
            indexesBuilder.append(_index);
            builder.append(kIndexesFieldName, indexesBuilder.arr());
        }
        auto opDescObj = builder.obj();

        stdx::lock_guard<Client> lk(*opCtx->getClient());
        // Show which index we're building in the curop display.
        auto curOp = CurOp::get(opCtx);
        curOp->setLogicalOp_inlock(LogicalOp::opCommand);
        curOp->setNS_inlock(ns.ns());
        curOp->setOpDescription_inlock(opDescObj);
    }

    // Ignore uniqueness constraint violations when relaxed (on secondaries). Secondaries can
    // complete index builds in the middle of batches, which creates the potential for finding
    // duplicate key violations where there otherwise would be none at consistent states.
    if (_indexConstraints == IndexConstraints::kRelax) {
        indexer.ignoreUniqueConstraint();
    }

    Status status = Status::OK();

    {
        TimestampBlock tsBlock(opCtx, _initIndexTs);
        status = writeConflictRetry(opCtx, "Init index build", ns.ns(), [&] {
            return indexer
                .init(
                    opCtx, coll, _index, MultiIndexBlock::makeTimestampedIndexOnInitFn(opCtx, coll))
                .getStatus();
        });
    }

    if (status == ErrorCodes::IndexAlreadyExists ||
        (status == ErrorCodes::IndexOptionsConflict &&
         _indexConstraints == IndexConstraints::kRelax)) {
        LOG(1) << "Ignoring indexing error: " << redact(status);

        // Must set this in case anyone is waiting for this build.
        if (dbLock) {
            _setBgIndexStarting();
        }
        return Status::OK();
    }
    if (!status.isOK()) {
        return status;
    }

    if (buildInBackground) {
        invariant(dbLock);

        _setBgIndexStarting();
        opCtx->recoveryUnit()->abandonSnapshot();

        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        dbLock->relockWithMode(MODE_IX);
    }

    {
        Lock::CollectionLock collLock(opCtx->lockState(), ns.ns(), MODE_IX);
        // WriteConflict exceptions and statuses are not expected to escape this method.
        status = indexer.insertAllDocumentsInCollection(opCtx, coll);
    }
    if (!status.isOK()) {
        return status;
    }

    if (buildInBackground) {
        {
            // Perform the first drain while holding an intent lock.
            Lock::CollectionLock collLock(opCtx->lockState(), ns.ns(), MODE_IX);

            // Read at a point in time so that the drain, which will timestamp writes at
            // lastApplied, can never commit writes earlier than its read timestamp.
            status = indexer.drainBackgroundWrites(opCtx, RecoveryUnit::ReadSource::kNoOverlap);
        }
        if (!status.isOK()) {
            return status;
        }

        // Perform the second drain while stopping inserts into the collection.
        {
            Lock::CollectionLock colLock(opCtx->lockState(), ns.ns(), MODE_S);
            status = indexer.drainBackgroundWrites(opCtx);
        }
        if (!status.isOK()) {
            return status;
        }

        opCtx->recoveryUnit()->abandonSnapshot();

        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        dbLock->relockWithMode(MODE_X);

        // Perform the third and final drain after releasing a shared lock and reacquiring an
        // exclusive lock on the database.
        status = indexer.drainBackgroundWrites(opCtx);
        if (!status.isOK()) {
            return status;
        }

        // Only perform constraint checking when enforced (on primaries).
        if (_indexConstraints == IndexConstraints::kEnforce) {
            status = indexer.checkConstraints(opCtx);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    status = writeConflictRetry(opCtx, "Commit index build", ns.ns(), [opCtx, coll, &indexer, &ns] {
        WriteUnitOfWork wunit(opCtx);
        auto status = indexer.commit(opCtx,
                                     coll,
                                     [opCtx, coll, &ns](const BSONObj& indexSpec) {
                                         opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                                             opCtx, ns, *(coll->uuid()), indexSpec, false);
                                     },
                                     MultiIndexBlock::kNoopOnCommitFn);
        if (!status.isOK()) {
            return status;
        }

        IndexTimestampHelper::setGhostCommitTimestampForCatalogWrite(opCtx, ns);
        wunit.commit();
        return Status::OK();
    });
    if (!status.isOK()) {
        return status;
    }

    if (buildInBackground) {
        invariant(opCtx->lockState()->isDbLockedForMode(ns.db(), MODE_X),
                  str::stream() << "Database not locked in exclusive mode after committing "
                                   "background index: "
                                << ns.ns()
                                << ": "
                                << _index);
        auto databaseHolder = DatabaseHolder::get(opCtx);
        auto reloadDb = databaseHolder->getDb(opCtx, ns.db());
        fassert(28553, reloadDb);
        fassert(28554, reloadDb->getCollection(opCtx, ns));
    }

    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}

}  // namespace mongo
