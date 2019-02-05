// #file dbtests.cpp : Runs db unit tests.
//


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

#include "mongo/platform/basic.h"

#include "mongo/dbtests/dbtests.h"

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/db/wire_version.h"
#include "mongo/dbtests/framework.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/startup_test.h"
#include "mongo/util/text.h"

namespace mongo {
namespace dbtests {
namespace {
const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;
}  // namespace

void initWireSpec() {
    WireSpec& spec = WireSpec::instance();

    // Accept from internal clients of the same version, as in upgrade featureCompatibilityVersion.
    spec.incomingInternalClient.minWireVersion = LATEST_WIRE_VERSION;
    spec.incomingInternalClient.maxWireVersion = LATEST_WIRE_VERSION;

    // Accept from any version external client.
    spec.incomingExternalClient.minWireVersion = RELEASE_2_4_AND_BEFORE;
    spec.incomingExternalClient.maxWireVersion = LATEST_WIRE_VERSION;

    // Connect to servers of the same version, as in upgrade featureCompatibilityVersion.
    spec.outgoing.minWireVersion = LATEST_WIRE_VERSION;
    spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;
}

Status createIndex(OperationContext* opCtx, StringData ns, const BSONObj& keys, bool unique) {
    BSONObjBuilder specBuilder;
    specBuilder.append("name", DBClientBase::genIndexName(keys));
    specBuilder.append("ns", ns);
    specBuilder.append("key", keys);
    specBuilder.append("v", static_cast<int>(kIndexVersion));
    if (unique) {
        specBuilder.appendBool("unique", true);
    }
    return createIndexFromSpec(opCtx, ns, specBuilder.done());
}

Status createIndexFromSpec(OperationContext* opCtx, StringData ns, const BSONObj& spec) {
    AutoGetOrCreateDb autoDb(opCtx, nsToDatabaseSubstring(ns), MODE_X);
    Collection* coll;
    {
        WriteUnitOfWork wunit(opCtx);
        coll = autoDb.getDb()->getOrCreateCollection(opCtx, NamespaceString(ns));
        invariant(coll);
        wunit.commit();
    }
    MultiIndexBlock indexer(opCtx, coll);
    Status status = indexer.init(spec, MultiIndexBlock::kNoopOnInitFn).getStatus();
    if (status == ErrorCodes::IndexAlreadyExists) {
        return Status::OK();
    }
    if (!status.isOK()) {
        return status;
    }
    status = indexer.insertAllDocumentsInCollection();
    if (!status.isOK()) {
        return status;
    }
    status = indexer.checkConstraints();
    if (!status.isOK()) {
        return status;
    }
    WriteUnitOfWork wunit(opCtx);
    ASSERT_OK(
        indexer.commit(MultiIndexBlock::kNoopOnCreateEachFn, MultiIndexBlock::kNoopOnCommitFn));
    wunit.commit();
    return Status::OK();
}

WriteContextForTests::WriteContextForTests(OperationContext* opCtx, StringData ns)
    : _opCtx(opCtx), _nss(ns) {
    // Lock the database and collection
    _autoCreateDb.emplace(opCtx, _nss.db(), MODE_IX);
    _collLock.emplace(opCtx->lockState(), _nss.ns(), MODE_IX);

    const bool doShardVersionCheck = false;

    _clientContext.emplace(opCtx, _nss.ns(), doShardVersionCheck);
    invariant(_autoCreateDb->getDb() == _clientContext->db());

    // If the collection exists, there is no need to lock into stronger mode
    if (getCollection())
        return;

    // If the database was just created, it is already locked in MODE_X so we can skip the relocking
    // code below
    if (_autoCreateDb->justCreated()) {
        dassert(opCtx->lockState()->isDbLockedForMode(_nss.db(), MODE_X));
        return;
    }

    // If the collection doesn't exists, put the context in a state where the database is locked in
    // MODE_X so that the collection can be created
    _clientContext.reset();
    _collLock.reset();
    _autoCreateDb.reset();
    _autoCreateDb.emplace(opCtx, _nss.db(), MODE_X);

    _clientContext.emplace(opCtx, _nss.ns(), _autoCreateDb->getDb());
    invariant(_autoCreateDb->getDb() == _clientContext->db());
}

}  // namespace dbtests
}  // namespace mongo


int dbtestsMain(int argc, char** argv, char** envp) {
    ::mongo::setTestCommandsEnabled(true);
    ::mongo::setupSynchronousSignalHandlers();
    mongo::dbtests::initWireSpec();

    mongo::runGlobalInitializersOrDie(argc, argv, envp);
    serverGlobalParams.featureCompatibility.setVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42);
    repl::ReplSettings replSettings;
    replSettings.setOplogSizeBytes(10 * 1024 * 1024);
    setGlobalServiceContext(ServiceContext::make());

    const auto service = getGlobalServiceContext();
    service->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongod>(service));

    auto logicalClock = stdx::make_unique<LogicalClock>(service);
    LogicalClock::set(service, std::move(logicalClock));

    auto fastClock = stdx::make_unique<ClockSourceMock>();
    // Timestamps are split into two 32-bit integers, seconds and "increments". Currently (but
    // maybe not for eternity), a Timestamp with a value of `0` seconds is always considered
    // "null" by `Timestamp::isNull`, regardless of its increment value. Ticking the
    // `ClockSourceMock` only bumps the "increment" counter, thus by default, generating "null"
    // timestamps. Bumping by one second here avoids any accidental interpretations.
    fastClock->advance(Seconds(1));
    service->setFastClockSource(std::move(fastClock));

    auto preciseClock = stdx::make_unique<ClockSourceMock>();
    // See above.
    preciseClock->advance(Seconds(1));
    service->setPreciseClockSource(std::move(preciseClock));

    service->setTransportLayer(
        transport::TransportLayerManager::makeAndStartDefaultEgressTransportLayer());

    repl::ReplicationCoordinator::set(
        service,
        std::unique_ptr<repl::ReplicationCoordinator>(
            new repl::ReplicationCoordinatorMock(service, replSettings)));
    repl::ReplicationCoordinator::get(service)
        ->setFollowerMode(repl::MemberState::RS_PRIMARY)
        .ignore();

    auto storageMock = stdx::make_unique<repl::StorageInterfaceMock>();
    repl::DropPendingCollectionReaper::set(
        service, stdx::make_unique<repl::DropPendingCollectionReaper>(storageMock.get()));

    AuthorizationManager::get(service)->setAuthEnabled(false);
    ScriptEngine::setup();
    StartupTest::runTests();
    return mongo::dbtests::runDbTests(argc, argv);
}

#if defined(_WIN32)
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through the argv() and envp() members.  This enables dbtestsMain()
// to process UTF-8 encoded arguments and environment variables without regard to platform.
int wmain(int argc, wchar_t* argvW[], wchar_t* envpW[]) {
    WindowsCommandLine wcl(argc, argvW, envpW);
    int exitCode = dbtestsMain(argc, wcl.argv(), wcl.envp());
    quickExit(exitCode);
}
#else
int main(int argc, char* argv[], char** envp) {
    int exitCode = dbtestsMain(argc, argv, envp);
    quickExit(exitCode);
}
#endif
