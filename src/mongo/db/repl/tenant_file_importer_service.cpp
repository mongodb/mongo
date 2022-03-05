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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/db/repl/tenant_file_importer_service.h"

#include <fmt/format.h>

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo::repl {

using namespace fmt::literals;
using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutor;
using executor::ThreadPoolTaskExecutor;

namespace {

const auto _TenantFileImporterService =
    ServiceContext::declareDecoration<TenantFileImporterService>();

const ReplicaSetAwareServiceRegistry::Registerer<TenantFileImporterService>
    _TenantFileImporterServiceRegisterer("TenantFileImporterService");

}  // namespace

TenantFileImporterService* TenantFileImporterService::get(ServiceContext* serviceContext) {
    return &_TenantFileImporterService(serviceContext);
}

void TenantFileImporterService::onStartup(OperationContext*) {
    auto net = executor::makeNetworkInterface("TenantFileImporterService-TaskExecutor");
    auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(net.get());
    _executor = std::make_shared<ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
    _executor->startup();
}

void TenantFileImporterService::startMigration(const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    _reset(lk);
    _migrationId = migrationId;
    _scopedExecutor = std::make_shared<executor::ScopedTaskExecutor>(
        _executor,
        Status{ErrorCodes::CallbackCanceled, "TenantFileImporterService executor cancelled"});
    _state.setState(ImporterState::State::kCopyingFiles);
}

void TenantFileImporterService::learnedFilename(const UUID& migrationId,
                                                const BSONObj& metadataDoc) {
    auto opCtx = cc().getOperationContext();
    {
        stdx::lock_guard lk(_mutex);
        uassert(8423347,
                "Called learnedFilename with migrationId {}, but {} is active"_format(
                    migrationId.toString(), _migrationId ? _migrationId->toString() : "(null)"),
                migrationId == _migrationId);
    }

    try {
        // TODO (SERVER-62734): Do this work asynchronously on the executor.
        shard_merge_utils::cloneFile(opCtx, metadataDoc);
    } catch (const DBException& ex) {
        LOGV2_ERROR(6229306,
                    "Error cloning files",
                    "migrationUUID"_attr = migrationId,
                    "error"_attr = ex.toStatus());
        // TODO (SERVER-63390): On error, vote shard merge abort to recipient primary.
    }
}

void TenantFileImporterService::learnedAllFilenames(const UUID& migrationId) {
    {
        stdx::lock_guard lk(_mutex);
        // TODO: try uassert
        if (!_state.is(ImporterState::State::kCopyingFiles)) {
            LOGV2_WARNING(8423346,
                          "Called learnedAllFilenames in wrong state",
                          "state"_attr = _state.toString());
            return;
        }

        uassert(8423345,
                "Called learnedAllFilenames with migrationId {}, but {} is active"_format(
                    migrationId.toString(), _migrationId ? _migrationId->toString() : "(null)"),
                migrationId == _migrationId);

        _state.setState(ImporterState::State::kCopiedFiles);
    }

    auto opCtx = cc().getOperationContext();
    // TODO SERVER-63789: Revisit use of AllowLockAcquisitionOnTimestampedUnitOfWork and
    // remove if possible.
    // No other threads will try to acquire conflicting locks: we are acquiring
    // database/collection locks for new tenants.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());
    shard_merge_utils::importCopiedFiles(opCtx, migrationId);

    // TODO (SERVER-62734): Keep count of files remaining to import, wait before voting.
    _voteImportedFiles();

    stdx::lock_guard lk(_mutex);
    _state.setState(ImporterState::State::kImportedFiles);
}

void TenantFileImporterService::reset() {
    stdx::lock_guard lk(_mutex);
    _reset(lk);
}

void TenantFileImporterService::_voteImportedFiles() {
    auto migrationId = [&] {
        stdx::lock_guard lk(_mutex);
        uassert(8423344, "Called _voteImportedFiles with null _migrationId", _migrationId);
        return *_migrationId;
    }();

    auto replCoord = ReplicationCoordinator::get(getGlobalServiceContext());
    // Call the command on the primary (which is self if this node is primary).
    auto primary = replCoord->getCurrentPrimaryHostAndPort();
    if (primary.empty()) {
        LOGV2_WARNING(
            6113406,
            "No primary for recipientVoteImportedFiles command, cannot continue migration",
            "migrationId"_attr = migrationId);
        return;
    }

    RecipientVoteImportedFiles cmd(migrationId, replCoord->getMyHostAndPort(), true /* success */);

    executor::RemoteCommandRequest request(primary, "admin", cmd.toBSON({}), nullptr);
    request.sslMode = transport::kGlobalSSLMode;
    auto scheduleResult =
        (*_scopedExecutor)
            ->scheduleRemoteCommand(
                request, [](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
                    if (!args.response.isOK()) {
                        LOGV2_WARNING(6113405,
                                      "recipientVoteImportedFiles command failed",
                                      "error"_attr = redact(args.response.status));
                        return;
                    }
                    auto status = getStatusFromCommandResult(args.response.data);
                    if (!status.isOK()) {
                        LOGV2_WARNING(6113404,
                                      "recipientVoteImportedFiles command failed",
                                      "error"_attr = redact(status));
                    }
                });
    if (!scheduleResult.isOK()) {
        LOGV2_WARNING(6113403,
                      "Failed to schedule recipientVoteImportedFiles command on primary",
                      "status"_attr = scheduleResult.getStatus());
    }
}

void TenantFileImporterService::_reset(WithLock lk) {
    _scopedExecutor.reset();  // Shuts down and joins the executor.
    _migrationId.reset();
    _state.setState(ImporterState::State::kUninitialized);
}
}  // namespace mongo::repl
