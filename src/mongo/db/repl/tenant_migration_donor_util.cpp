/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/platform/basic.h"
#include "mongo/util/str.h"

#include "mongo/db/repl/tenant_migration_donor_util.h"

#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"

namespace mongo {

// Failpoint that will cause recoverTenantMigrationAccessBlockers to return early.
MONGO_FAIL_POINT_DEFINE(skipRecoverTenantMigrationAccessBlockers);

namespace tenant_migration_donor {

namespace {

const char kThreadNamePrefix[] = "TenantMigrationWorker-";
const char kPoolName[] = "TenantMigrationWorkerThreadPool";
const char kNetName[] = "TenantMigrationWorkerNetwork";

const auto donorStateDocToDeleteDecoration = OperationContext::declareDecoration<BSONObj>();

}  // namespace

TenantMigrationDonorDocument parseDonorStateDocument(const BSONObj& doc) {
    auto donorStateDoc =
        TenantMigrationDonorDocument::parse(IDLParserErrorContext("donorStateDoc"), doc);

    if (donorStateDoc.getExpireAt()) {
        uassert(ErrorCodes::BadValue,
                "contains \"expireAt\" but the migration has not committed or aborted",
                donorStateDoc.getState() == TenantMigrationDonorStateEnum::kCommitted ||
                    donorStateDoc.getState() == TenantMigrationDonorStateEnum::kAborted);
    }

    const std::string errmsg = str::stream() << "invalid donor state doc " << doc;

    switch (donorStateDoc.getState()) {
        case TenantMigrationDonorStateEnum::kUninitialized:
            break;
        case TenantMigrationDonorStateEnum::kDataSync:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    !donorStateDoc.getBlockTimestamp() && !donorStateDoc.getCommitOrAbortOpTime() &&
                        !donorStateDoc.getAbortReason());
            break;
        case TenantMigrationDonorStateEnum::kBlocking:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    donorStateDoc.getBlockTimestamp() && !donorStateDoc.getCommitOrAbortOpTime() &&
                        !donorStateDoc.getAbortReason());
            break;
        case TenantMigrationDonorStateEnum::kCommitted:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    donorStateDoc.getBlockTimestamp() && donorStateDoc.getCommitOrAbortOpTime() &&
                        !donorStateDoc.getAbortReason());
            break;
        case TenantMigrationDonorStateEnum::kAborted:
            uassert(ErrorCodes::BadValue, errmsg, donorStateDoc.getAbortReason());
            break;
        default:
            MONGO_UNREACHABLE;
    }

    return donorStateDoc;
}

std::shared_ptr<executor::TaskExecutor> getTenantMigrationDonorExecutor() {
    static Mutex mutex = MONGO_MAKE_LATCH("TenantMigrationDonorUtilExecutor::_mutex");
    static std::shared_ptr<executor::TaskExecutor> executor;

    stdx::lock_guard<Latch> lg(mutex);
    if (!executor) {
        ThreadPool::Options tpOptions;
        tpOptions.threadNamePrefix = kThreadNamePrefix;
        tpOptions.poolName = kPoolName;
        tpOptions.minThreads = 0;
        tpOptions.maxThreads = 16;

        executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
            std::make_unique<ThreadPool>(tpOptions), executor::makeNetworkInterface(kNetName));
        executor->startup();
    }

    return executor;
}

void checkIfCanReadOrBlock(OperationContext* opCtx, StringData dbName) {
    auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName);

    if (!mtab) {
        return;
    }

    auto readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    auto targetTimestamp = [&]() -> boost::optional<Timestamp> {
        if (auto afterClusterTime = readConcernArgs.getArgsAfterClusterTime()) {
            return afterClusterTime->asTimestamp();
        }
        if (auto atClusterTime = readConcernArgs.getArgsAtClusterTime()) {
            return atClusterTime->asTimestamp();
        }
        if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
            return repl::StorageInterface::get(opCtx)->getPointInTimeReadTimestamp(opCtx);
        }
        return boost::none;
    }();

    if (targetTimestamp) {
        mtab->checkIfCanDoClusterTimeReadOrBlock(opCtx, targetTimestamp.get());
    }
}

void checkIfLinearizableReadWasAllowedOrThrow(OperationContext* opCtx, StringData dbName) {
    if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
        repl::ReadConcernLevel::kLinearizableReadConcern) {
        if (auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                            .getTenantMigrationAccessBlockerForDbName(dbName)) {
            mtab->checkIfLinearizableReadWasAllowedOrThrow(opCtx);
        }
    }
}

void onWriteToDatabase(OperationContext* opCtx, StringData dbName) {
    auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName);

    if (mtab) {
        mtab->checkIfCanWriteOrThrow();
    }
}

void recoverTenantMigrationAccessBlockers(OperationContext* opCtx) {
    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext()).shutDown();

    if (MONGO_unlikely(skipRecoverTenantMigrationAccessBlockers.shouldFail())) {
        return;
    }

    PersistentTaskStore<TenantMigrationDonorDocument> store(
        NamespaceString::kTenantMigrationDonorsNamespace);
    Query query;

    store.forEach(opCtx, query, [&](const TenantMigrationDonorDocument& doc) {
        auto mtab = std::make_shared<TenantMigrationAccessBlocker>(
            opCtx->getServiceContext(),
            getTenantMigrationDonorExecutor(),
            doc.getTenantId().toString(),
            doc.getRecipientConnectionString().toString());

        TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
            .add(doc.getTenantId(), mtab);

        switch (doc.getState()) {
            case TenantMigrationDonorStateEnum::kDataSync:
                break;
            case TenantMigrationDonorStateEnum::kBlocking:
                invariant(doc.getBlockTimestamp());
                mtab->startBlockingWrites();
                mtab->startBlockingReadsAfter(doc.getBlockTimestamp().get());
                break;
            case TenantMigrationDonorStateEnum::kCommitted:
                invariant(doc.getBlockTimestamp());
                mtab->startBlockingWrites();
                mtab->startBlockingReadsAfter(doc.getBlockTimestamp().get());
                mtab->commit(doc.getCommitOrAbortOpTime().get());
                break;
            case TenantMigrationDonorStateEnum::kAborted:
                if (doc.getBlockTimestamp()) {
                    mtab->startBlockingWrites();
                    mtab->startBlockingReadsAfter(doc.getBlockTimestamp().get());
                }
                mtab->abort(doc.getCommitOrAbortOpTime().get());
                break;
            default:
                MONGO_UNREACHABLE;
        }
        return true;
    });
}

class MigrationConflictHandler : public std::enable_shared_from_this<MigrationConflictHandler> {
public:
    MigrationConflictHandler(std::shared_ptr<RequestExecutionContext> rec,
                             unique_function<Future<void>()> callable)
        : _rec(std::move(rec)), _callable(std::move(callable)) {}

    Future<void> run() try {
        checkIfCanReadOrBlock(_rec->getOpCtx(), _rec->getRequest().getDatabase());
        // callable will modify replyBuilder.
        return _callable()
            .then([this, anchor = shared_from_this()] { _checkReplyForTenantMigrationConflict(); })
            .onError<ErrorCodes::TenantMigrationConflict>(
                [this, anchor = shared_from_this()](Status status) {
                    _handleTenantMigrationConflict(std::move(status));
                });
    } catch (const DBException& e) {
        return e.toStatus();
    }

private:
    void _checkReplyForTenantMigrationConflict() {
        auto replyBodyBuilder = _rec->getReplyBuilder()->getBodyBuilder();

        // getStatusFromWriteCommandReply expects an 'ok' field.
        CommandHelpers::extractOrAppendOk(replyBodyBuilder);

        // Commands such as insert, update, delete, and applyOps return the result as a status
        // rather than throwing.
        const auto status = getStatusFromWriteCommandReply(replyBodyBuilder.asTempObj());

        // Only throw `TenantMigrationConflict` exceptions.
        if (status == ErrorCodes::TenantMigrationConflict)
            internalAssert(status);
    }

    void _handleTenantMigrationConflict(Status status) {
        auto migrationConflictInfo = status.extraInfo<TenantMigrationConflictInfo>();
        invariant(migrationConflictInfo);

        if (auto mtab = migrationConflictInfo->getTenantMigrationAccessBlocker()) {
            uassertStatusOK(mtab->waitUntilCommittedOrAborted(_rec->getOpCtx()));
        }
    }

    const std::shared_ptr<RequestExecutionContext> _rec;
    const unique_function<Future<void>()> _callable;
};

Future<void> migrationConflictHandler(std::shared_ptr<RequestExecutionContext> rec,
                                      unique_function<Future<void>()> callable) {
    return std::make_shared<MigrationConflictHandler>(std::move(rec), std::move(callable))->run();
}

}  // namespace tenant_migration_donor

}  // namespace mongo
