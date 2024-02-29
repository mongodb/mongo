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

/**
 * Runs db unit tests.
 */

#include <boost/move/utility_core.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/db/session_manager_mongod.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/wire_version.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/dbtests/framework.h"
#include "mongo/scripting/engine.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/text.h"  // IWYU pragma: keep
#include "mongo/util/version/releases.h"

namespace mongo {
namespace dbtests {
namespace {

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

ServiceContext::ConstructorActionRegisterer registerWireSpec{
    "RegisterWireSpec", [](ServiceContext* service) {
        WireSpec::Specification spec;

        // Accept from any version external client.
        spec.incomingExternalClient.minWireVersion = RELEASE_2_4_AND_BEFORE;
        spec.incomingExternalClient.maxWireVersion = LATEST_WIRE_VERSION;

        // Accept from internal clients of the same version, as in upgrade
        // featureCompatibilityVersion.
        spec.incomingInternalClient.minWireVersion = LATEST_WIRE_VERSION;
        spec.incomingInternalClient.maxWireVersion = LATEST_WIRE_VERSION;

        // Connect to servers of the same version, as in upgrade featureCompatibilityVersion.
        spec.outgoing.minWireVersion = LATEST_WIRE_VERSION;
        spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;

        WireSpec::getWireSpec(service).initialize(std::move(spec));
    }};

}  // namespace

Status createIndex(OperationContext* opCtx, StringData ns, const BSONObj& keys, bool unique) {
    BSONObjBuilder specBuilder;
    specBuilder.append("name", DBClientBase::genIndexName(keys));
    specBuilder.append("key", keys);
    specBuilder.append("v", static_cast<int>(kIndexVersion));
    if (unique) {
        specBuilder.appendBool("unique", true);
    }
    return createIndexFromSpec(opCtx, ns, specBuilder.done());
}

Status createIndexFromSpec(OperationContext* opCtx, StringData ns, const BSONObj& spec) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(ns);
    AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
    {
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);
        WriteUnitOfWork wunit(opCtx);
        auto coll =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForMetadataWrite(opCtx, nss);
        if (!coll) {
            auto db = autoDb.ensureDbExists(opCtx);
            invariant(db);
            coll = db->createCollection(opCtx, NamespaceString::createNamespaceString_forTest(ns));
        }
        invariant(coll);
        wunit.commit();
    }
    MultiIndexBlock indexer;
    ScopeGuard abortOnExit([&] {
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);
        CollectionWriter collection(opCtx, nss);
        WriteUnitOfWork wunit(opCtx);
        indexer.abortIndexBuild(opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn);
        wunit.commit();
    });
    auto status = Status::OK();
    {
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);
        CollectionWriter collection(opCtx, nss);
        status = indexer
                     .init(opCtx,
                           collection,
                           spec,
                           [opCtx](const std::vector<BSONObj>& specs) -> Status {
                               if (shard_role_details::getRecoveryUnit(opCtx)
                                       ->getCommitTimestamp()
                                       .isNull()) {
                                   return shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(
                                       Timestamp(1, 1));
                               }
                               return Status::OK();
                           })
                     .getStatus();
        if (status == ErrorCodes::IndexAlreadyExists) {
            return Status::OK();
        }
        if (!status.isOK()) {
            return status;
        }
    }
    {
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        // Using CollectionWriter for convenience here.
        CollectionWriter collection(opCtx, nss);
        status = indexer.insertAllDocumentsInCollection(opCtx, collection.get());
        if (!status.isOK()) {
            return status;
        }
    }
    {
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);
        CollectionWriter collection(opCtx, nss);
        status = indexer.retrySkippedRecords(opCtx, collection.get());
        if (!status.isOK()) {
            return status;
        }

        status = indexer.checkConstraints(opCtx, collection.get());
        if (!status.isOK()) {
            return status;
        }
        WriteUnitOfWork wunit(opCtx);
        ASSERT_OK(indexer.commit(opCtx,
                                 collection.getWritableCollection(opCtx),
                                 MultiIndexBlock::kNoopOnCreateEachFn,
                                 MultiIndexBlock::kNoopOnCommitFn));
        ASSERT_OK(shard_role_details::getRecoveryUnit(opCtx)->setTimestamp(Timestamp(1, 1)));
        wunit.commit();
    }
    abortOnExit.dismiss();
    return Status::OK();
}

WriteContextForTests::WriteContextForTests(OperationContext* opCtx, StringData ns)
    : _opCtx(opCtx), _nss(NamespaceString::createNamespaceString_forTest(ns)) {
    // Lock the database and collection
    _autoDb.emplace(opCtx, _nss.dbName(), MODE_IX);
    _collLock.emplace(opCtx, _nss, MODE_X);

    const bool doShardVersionCheck = false;

    _clientContext.emplace(opCtx, _nss, doShardVersionCheck);
    auto db = _autoDb->ensureDbExists(opCtx);
    invariant(db, _nss.toStringForErrorMsg());
    invariant(db == _clientContext->db());
}

int dbtestsMain(int argc, char** argv) {
    ::mongo::setTestCommandsEnabled(true);
    ::mongo::TestingProctor::instance().setEnabled(true);
    ::mongo::setupSynchronousSignalHandlers();

    mongo::runGlobalInitializersOrDie(std::vector<std::string>(argv, argv + argc));
    // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
    repl::ReplSettings replSettings;
    replSettings.setOplogSizeBytes(10 * 1024 * 1024);
    setGlobalServiceContext(ServiceContext::make());

    const auto service = getGlobalServiceContext();
    service->getService()->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongod>());

    auto fastClock = std::make_unique<ClockSourceMock>();
    // Timestamps are split into two 32-bit integers, seconds and "increments". Currently (but
    // maybe not for eternity), a Timestamp with a value of `0` seconds is always considered
    // "null" by `Timestamp::isNull`, regardless of its increment value. Ticking the
    // `ClockSourceMock` only bumps the "increment" counter, thus by default, generating "null"
    // timestamps. Bumping by one second here avoids any accidental interpretations.
    fastClock->advance(Seconds(1));
    service->setFastClockSource(std::move(fastClock));

    auto preciseClock = std::make_unique<ClockSourceMock>();
    // See above.
    preciseClock->advance(Seconds(1));
    CursorManager::get(service)->setPreciseClockSource(preciseClock.get());
    service->setPreciseClockSource(std::move(preciseClock));

    service->setTransportLayerManager(
        transport::TransportLayerManagerImpl::makeAndStartDefaultEgressTransportLayer());

    repl::ReplicationCoordinator::set(
        service,
        std::unique_ptr<repl::ReplicationCoordinator>(
            new repl::ReplicationCoordinatorMock(service, replSettings)));
    repl::ReplicationCoordinator::get(service)
        ->setFollowerMode(repl::MemberState::RS_PRIMARY)
        .ignore();

    auto storageMock = std::make_unique<repl::StorageInterfaceMock>();
    repl::DropPendingCollectionReaper::set(
        service, std::make_unique<repl::DropPendingCollectionReaper>(storageMock.get()));

    AuthorizationManager::get(service->getService())->setAuthEnabled(false);
    ScriptEngine::setup(ExecutionEnvironment::Server);
    return mongo::dbtests::runDbTests(argc, argv);
}

}  // namespace dbtests
}  // namespace mongo

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables dbtestsMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[]) {
    mongo::quickExit(
        mongo::dbtests::dbtestsMain(argc, mongo::WindowsCommandLine(argc, argvW).argv()));
}
#else
int main(int argc, char* argv[]) {
    mongo::quickExit(mongo::dbtests::dbtestsMain(argc, argv));
}
#endif
