/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/migration_batch_inserter.h"

#include "mongo/db/s/sharding_statistics.h"
#include "mongo/util/concurrency/ticketholder.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kShardingMigration

namespace mongo {

namespace {

void checkOutSessionAndVerifyTxnState(OperationContext* opCtx) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    mongoDSessionCatalog->checkOutUnscopedSession(opCtx);
    TransactionParticipant::get(opCtx).beginOrContinue(opCtx,
                                                       {*opCtx->getTxnNumber()},
                                                       boost::none /* autocommit */,
                                                       boost::none /* startTransaction */);
}

template <typename Callable>
constexpr bool returnsVoid() {
    return std::is_void_v<std::invoke_result_t<Callable>>;
}

// Yields the checked out session before running the given function. If the function runs without
// throwing, will reacquire the session and verify it is still valid to proceed with the migration.
template <typename Callable, std::enable_if_t<!returnsVoid<Callable>(), int> = 0>
auto runWithoutSession(OperationContext* opCtx, Callable callable) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    mongoDSessionCatalog->checkInUnscopedSession(opCtx,
                                                 OperationContextSession::CheckInReason::kYield);

    auto retVal = callable();

    // The below code can throw, so it cannot run in a scope guard.
    opCtx->checkForInterrupt();
    checkOutSessionAndVerifyTxnState(opCtx);

    return retVal;
}

// Same as runWithoutSession above but takes a void function.
template <typename Callable, std::enable_if_t<returnsVoid<Callable>(), int> = 0>
void runWithoutSession(OperationContext* opCtx, Callable callable) {
    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    mongoDSessionCatalog->checkInUnscopedSession(opCtx,
                                                 OperationContextSession::CheckInReason::kYield);

    callable();

    // The below code can throw, so it cannot run in a scope guard.
    opCtx->checkForInterrupt();
    checkOutSessionAndVerifyTxnState(opCtx);
}
}  // namespace


void MigrationBatchInserter::onCreateThread(const std::string& threadName) {
    Client::initThread(threadName, getGlobalServiceContext(), nullptr);
}

void MigrationBatchInserter::run(Status status) const try {
    // Run is passed in a non-ok status if this function runs inline.
    // That happens if we schedule this task on a ThreadPool that is
    // already shutdown.  If we were to schedule a task on a shutdown ThreadPool,
    // then there is a logic error in our code.  Therefore, we assert that here.

    invariant(status.isOK());
    auto arr = _batch["objects"].Obj();
    if (arr.isEmpty())
        return;

    auto executor =
        Grid::get(_innerOpCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();

    auto applicationOpCtx = CancelableOperationContext(
        cc().makeOperationContext(), _innerOpCtx->getCancellationToken(), executor);

    auto opCtx = applicationOpCtx.get();

    auto assertNotAborted = [&]() {
        {
            stdx::lock_guard<Client> lk(*_outerOpCtx->getClient());
            _outerOpCtx->checkForInterrupt();
        }
        opCtx->checkForInterrupt();
    };

    auto it = arr.begin();
    while (it != arr.end()) {
        int batchNumCloned = 0;
        int batchClonedBytes = 0;
        const int batchMaxCloned = migrateCloneInsertionBatchSize.load();

        assertNotAborted();

        write_ops::InsertCommandRequest insertOp(_nss);
        insertOp.getWriteCommandRequestBase().setOrdered(true);
        insertOp.setDocuments([&] {
            std::vector<BSONObj> toInsert;
            while (it != arr.end() && (batchMaxCloned <= 0 || batchNumCloned < batchMaxCloned)) {
                const auto& doc = *it;
                BSONObj docToClone = doc.Obj();
                toInsert.push_back(docToClone);
                batchNumCloned++;
                batchClonedBytes += docToClone.objsize();
                ++it;
            }
            return toInsert;
        }());

        {
            // Disable the schema validation (during document inserts and updates)
            // and any internal validation for opCtx for performInserts()
            DisableDocumentValidation documentValidationDisabler(
                opCtx,
                DocumentValidationSettings::kDisableSchemaValidation |
                    DocumentValidationSettings::kDisableInternalValidation);
            const auto reply =
                write_ops_exec::performInserts(opCtx, insertOp, OperationSource::kFromMigrate);
            for (unsigned long i = 0; i < reply.results.size(); ++i) {
                uassertStatusOKWithContext(
                    reply.results[i],
                    str::stream() << "Insert of " << insertOp.getDocuments()[i] << " failed.");
            }
            // Revert to the original DocumentValidationSettings for opCtx
        }

        persistUpdatedNumOrphans(opCtx, _collectionUuid, _range, batchNumCloned);
        _migrationProgress->updateMaxOptime(
            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());

        ShardingStatistics::get(opCtx).countDocsClonedOnRecipient.addAndFetch(batchNumCloned);
        ShardingStatistics::get(opCtx).countBytesClonedOnRecipient.addAndFetch(batchClonedBytes);
        LOGV2(6718408,
              "Incrementing numCloned count by {batchNumCloned} and numClonedBytes by "
              "{batchClonedBytes}",
              "batchNumCloned"_attr = batchNumCloned,
              "batchClonedBytes"_attr = batchClonedBytes);
        _migrationProgress->incNumCloned(batchNumCloned);
        _migrationProgress->incNumBytes(batchClonedBytes);

        if (_writeConcern.needToWaitForOtherNodes() && _threadCount == 1) {
            AdmissionContext admissionContext;
            if (auto ticket = _secondaryThrottleTicket->tryAcquire(&admissionContext)) {
                runWithoutSession(_outerOpCtx, [&] {
                    repl::ReplicationCoordinator::StatusAndDuration replStatus =
                        repl::ReplicationCoordinator::get(opCtx)->awaitReplication(
                            opCtx,
                            repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp(),
                            _writeConcern);
                    if (replStatus.status.code() == ErrorCodes::WriteConcernFailed) {
                        LOGV2_WARNING(22011,
                                      "secondaryThrottle on, but doc insert timed out; continuing",
                                      "migrationId"_attr = _migrationId.toBSON());
                    } else {
                        uassertStatusOK(replStatus.status);
                    }
                });
            } else {
                // Ticket should always be available unless thread pool max size 1 setting is not
                // being respected.
                dassert(false);
            }
        }

        sleepmillis(migrateCloneInsertionBatchDelayMS.load());
    }
} catch (const DBException& e) {
    stdx::lock_guard<Client> lk(*_innerOpCtx->getClient());
    _innerOpCtx->getServiceContext()->killOperation(lk, _innerOpCtx, ErrorCodes::Error(6718402));
    LOGV2(6718407,
          "Batch application failed: {error}",
          "Batch application failed",
          "error"_attr = e.toStatus());
}
}  // namespace mongo
