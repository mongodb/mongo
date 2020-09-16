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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/base/checked_cast.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/tenant_migration_recipient_entry_helpers.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {
namespace repl {

// Fails before waiting for the state doc to be majority replicated.
MONGO_FAIL_POINT_DEFINE(failWhilePersistingTenantMigrationRecipientInstanceStateDoc);
MONGO_FAIL_POINT_DEFINE(stopAfterPersistingTenantMigrationRecipientInstanceStateDoc);
MONGO_FAIL_POINT_DEFINE(stopAfterConnectingTenantMigrationRecipientInstance);
MONGO_FAIL_POINT_DEFINE(setTenantMigrationRecipientInstanceHostTimeout);

TenantMigrationRecipientService::TenantMigrationRecipientService(ServiceContext* serviceContext)
    : PrimaryOnlyService(serviceContext) {}

StringData TenantMigrationRecipientService::getServiceName() const {
    return kTenantMigrationRecipientServiceName;
}

NamespaceString TenantMigrationRecipientService::getStateDocumentsNS() const {
    return NamespaceString::kTenantMigrationRecipientsNamespace;
}

ThreadPool::Limits TenantMigrationRecipientService::getThreadPoolLimits() const {
    // TODO SERVER-50669: This will be replaced by a tunable server parameter.
    return ThreadPool::Limits();
}

std::shared_ptr<PrimaryOnlyService::Instance> TenantMigrationRecipientService::constructInstance(
    BSONObj initialStateDoc) const {
    return std::make_shared<TenantMigrationRecipientService::Instance>(initialStateDoc);
}

TenantMigrationRecipientService::Instance::Instance(BSONObj stateDoc)
    : PrimaryOnlyService::TypedInstance<Instance>(),
      _stateDoc(TenantMigrationRecipientDocument::parse(IDLParserErrorContext("recipientStateDoc"),
                                                        stateDoc)),
      _tenantId(_stateDoc.getDatabasePrefix().toString()),
      _migrationUuid(_stateDoc.getId()),
      _donorConnectionString(_stateDoc.getDonorConnectionString().toString()),
      _readPreference(_stateDoc.getReadPreference()) {}

std::unique_ptr<DBClientConnection> TenantMigrationRecipientService::Instance::_connectAndAuth(
    const HostAndPort& serverAddress, StringData applicationName, BSONObj authParams) {
    std::string errMsg;
    auto clientBase = ConnectionString(serverAddress).connect(applicationName, errMsg);
    if (!clientBase) {
        LOGV2_ERROR(4880400,
                    "Failed to connect to migration donor",
                    "tenantId"_attr = getTenantId(),
                    "migrationId"_attr = getMigrationUUID(),
                    "serverAddress"_attr = serverAddress,
                    "applicationName"_attr = applicationName,
                    "error"_attr = errMsg);
        uasserted(ErrorCodes::HostNotFound, errMsg);
    }
    // ConnectionString::connect() always returns a DBClientConnection in a unique_ptr of
    // DBClientBase type.
    std::unique_ptr<DBClientConnection> client(
        checked_cast<DBClientConnection*>(clientBase.release()));
    if (!authParams.isEmpty()) {
        client->auth(authParams);
    } else {
        // Tenant migration in production should always require auth.
        uassert(4880405, "No auth data provided to tenant migration", getTestCommandsEnabled());
    }

    return client;
}

SemiFuture<void> TenantMigrationRecipientService::Instance::_createAndConnectClients() {
    LOGV2_DEBUG(4880401,
                1,
                "Recipient migration service connecting clients",
                "tenantId"_attr = getTenantId(),
                "migrationId"_attr = getMigrationUUID(),
                "connectionString"_attr = _donorConnectionString,
                "readPreference"_attr = _readPreference,
                "authParams"_attr = redact(_authParams));
    auto connectionStringWithStatus = ConnectionString::parse(_donorConnectionString);
    if (!connectionStringWithStatus.isOK()) {
        LOGV2_ERROR(4880403,
                    "Failed to parse connection string",
                    "tenantId"_attr = getTenantId(),
                    "migrationId"_attr = getMigrationUUID(),
                    "connectionString"_attr = _donorConnectionString,
                    "error"_attr = connectionStringWithStatus.getStatus());

        return SemiFuture<void>::makeReady(connectionStringWithStatus.getStatus());
    }
    auto connectionString = std::move(connectionStringWithStatus.getValue());
    const auto& servers = connectionString.getServers();
    stdx::lock_guard lk(_mutex);
    _donorReplicaSetMonitor = ReplicaSetMonitor::createIfNeeded(
        connectionString.getSetName(), std::set<HostAndPort>(servers.begin(), servers.end()));
    Milliseconds findHostTimeout = ReplicaSetMonitorInterface::kDefaultFindHostTimeout;
    setTenantMigrationRecipientInstanceHostTimeout.execute([&](const BSONObj& data) {
        findHostTimeout = Milliseconds(data["findHostTimeoutMillis"].safeNumberLong());
    });
    return _donorReplicaSetMonitor->getHostOrRefresh(_readPreference, findHostTimeout)
        .thenRunOn(**_scopedExecutor)
        .then([this](const HostAndPort& serverAddress) {
            stdx::lock_guard lk(_mutex);
            auto applicationName =
                "TenantMigrationRecipient_" + getTenantId() + "_" + getMigrationUUID().toString();
            _client = _connectAndAuth(serverAddress, applicationName, _authParams);

            applicationName += "_fetcher";
            _oplogFetcherClient = _connectAndAuth(serverAddress, applicationName, _authParams);
        })
        .onError([this](const Status& status) {
            LOGV2_ERROR(4880404,
                        "Connecting to donor failed",
                        "tenantId"_attr = getTenantId(),
                        "migrationId"_attr = getMigrationUUID(),
                        "error"_attr = status);

            // Make sure we don't end up with a partially initialized set of connections.
            stdx::lock_guard lk(_mutex);
            _client = nullptr;
            _oplogFetcherClient = nullptr;
            return status;
        })
        .semi();
}

SharedSemiFuture<void> TenantMigrationRecipientService::Instance::_initializeStateDoc() {
    stdx::lock_guard lk(_mutex);
    // If the instance state is not 'kUninitialized', then the instance is restarted by step
    // up. So, skip persisting the state doc. And, PrimaryOnlyService::onStepUp() waits for
    // majority commit of the primary no-op oplog entry written by the node in the newer
    // term before scheduling the Instance::run(). So, it's also safe to assume that
    // instance's state document written in an older term on disk won't get rolled back for
    // step up case.
    if (_stateDoc.getState() != TenantMigrationRecipientStateEnum::kUninitialized) {
        return {Future<void>::makeReady()};
    }

    auto uniqueOpCtx = cc().makeOperationContext();
    auto opCtx = uniqueOpCtx.get();


    LOGV2_DEBUG(5081400,
                2,
                "Recipient migration service initializing state document",
                "tenantId"_attr = getTenantId(),
                "migrationId"_attr = getMigrationUUID(),
                "connectionString"_attr = _donorConnectionString,
                "readPreference"_attr = _readPreference);

    // Persist the state doc before starting the data sync.
    _stateDoc.setState(TenantMigrationRecipientStateEnum::kStarted);
    uassertStatusOK(tenantMigrationRecipientEntryHelpers::insertStateDoc(opCtx, _stateDoc));

    if (MONGO_unlikely(failWhilePersistingTenantMigrationRecipientInstanceStateDoc.shouldFail())) {
        LOGV2(4878500, "Persisting state doc failed due to fail point enabled.");
        uassert(ErrorCodes::NotWritablePrimary,
                "Persisting state doc failed - "
                "'failWhilePersistingTenantMigrationRecipientInstanceStateDoc' fail point active",
                false);
    }

    // Wait for the state doc to be majority replicated to make sure that the state doc doesn't
    // rollback.
    auto insertOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    return WaitForMajorityService::get(opCtx->getServiceContext()).waitUntilMajority(insertOpTime);
}

namespace {
constexpr std::int32_t stopFailPointErrorCode = 4880402;
void stopOnFailPoint(FailPoint* fp) {
    uassert(stopFailPointErrorCode,
            "Skipping remaining processing due to fail point",
            MONGO_likely(!fp->shouldFail()));
}
}  // namespace

void TenantMigrationRecipientService::Instance::interrupt(Status status) {
    stdx::lock_guard lk(_mutex);
    // Resolve any unresolved promises to avoid hanging.
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

void TenantMigrationRecipientService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {
    _scopedExecutor = executor;
    ExecutorFuture(**executor)
        .then([this]() { return _initializeStateDoc(); })
        .then([this] {
            stopOnFailPoint(&stopAfterPersistingTenantMigrationRecipientInstanceStateDoc);
            return _createAndConnectClients();
        })
        .then([this] {
            stopOnFailPoint(&stopAfterConnectingTenantMigrationRecipientInstance);
            stdx::lock_guard lk(_mutex);
            // The instance is marked as garbage collect if the migration is either
            // committed or aborted on donor side. So, don't start the recipient task if the
            // instance state doc is marked for garbage collect.
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Can't start the data sync as the state doc is already marked "
                                     "for garbage collect for migration uuid: "
                                  << getMigrationUUID(),
                    !_stateDoc.getGarbageCollect());

            // TODO SERVER-48808: Run cloners in MigrationServiceInstance
            // TODO SERVER-48811: Oplog fetching in MigrationServiceInstance
        })
        .getAsync([this](Status status) {
            LOGV2(4878501,
                  "Tenant Recipient data sync completed.",
                  "tenantId"_attr = getTenantId(),
                  "migrationId"_attr = getMigrationUUID(),
                  "error"_attr = status);

            stdx::lock_guard lk(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got here
                return;
            }

            if (status.isOK() || status.code() == stopFailPointErrorCode) {
                _completionPromise.emplaceValue();
            } else {
                _completionPromise.setError(status);
            }
        });
}

const UUID& TenantMigrationRecipientService::Instance::getMigrationUUID() const {
    return _migrationUuid;
}

const std::string& TenantMigrationRecipientService::Instance::getTenantId() const {
    return _tenantId;
}

}  // namespace repl
}  // namespace mongo
