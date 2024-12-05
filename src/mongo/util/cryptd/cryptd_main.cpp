/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/filesystem.hpp>

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/status.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/initialize_server_global_state.h"
#include "mongo/db/log_process_details.h"
#include "mongo/db/repl/replication_coordinator_noop.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/s/sharding_state.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cmdline_utils/censor_cmdline.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/cryptd/cryptd_options.h"
#include "mongo/util/cryptd/cryptd_options_gen.h"
#include "mongo/util/cryptd/cryptd_service_entry_point.h"
#include "mongo/util/cryptd/cryptd_watchdog.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers.h"
#include "mongo/util/str.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"
#include "mongo/util/version/releases.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {

#ifdef _WIN32
const ntservice::NtServiceDefaultStrings defaultServiceStrings = {
    L"MongoCryptD", L"MongoDB FLE Crypto", L"MongoDB Field Level Encryption Daemon"};
#endif

ServiceContext::ConstructorActionRegisterer registerWireSpec{
    "RegisterWireSpec", [](ServiceContext* service) {
        // For MongoCryptd, we set the minimum wire version to be 4.2
        WireSpec::Specification spec;
        spec.incomingInternalClient.minWireVersion = SHARDED_TRANSACTIONS;
        spec.incomingInternalClient.maxWireVersion = LATEST_WIRE_VERSION;
        spec.outgoing.minWireVersion = SHARDED_TRANSACTIONS;
        spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;
        spec.isInternalClient = true;

        WireSpec::getWireSpec(service).initialize(std::move(spec));
    }};

void createLockFile(ServiceContext* serviceContext) {
    auto& lockFile = StorageEngineLockFile::get(serviceContext);

    boost::filesystem::path orig_file(serverGlobalParams.pidFile);
    boost::filesystem::path file(boost::filesystem::absolute(orig_file));

    LOGV2(24225, "Using lock file", "file"_attr = file.generic_string());
    try {
        lockFile.emplace(file.parent_path().generic_string(), file.filename().generic_string());
    } catch (const std::exception& ex) {
        LOGV2_ERROR(24230,
                    "Unable to determine status of lock file in the data directory",
                    "file"_attr = file.generic_string(),
                    "error"_attr = ex.what());
        _exit(static_cast<int>(ExitCode::fail));
    }

    const auto openStatus = lockFile->open();
    if (!openStatus.isOK()) {
        LOGV2_ERROR(24231, "Failed to open pid file, exiting", "error"_attr = openStatus);
        _exit(static_cast<int>(ExitCode::fail));
    }

    CryptdPidFile pidFile;
    pidFile.setPort(serverGlobalParams.port);
    pidFile.setPid(ProcessId::getCurrent().asInt64());

    auto str = tojson(pidFile.toBSON(), JsonStringFormat::LegacyStrict);

    const auto writeStatus = lockFile->writeString(str);
    if (!writeStatus.isOK()) {
        LOGV2_ERROR(24232, "Failed to write pid file, exiting", "error"_attr = writeStatus);
        _exit(static_cast<int>(ExitCode::fail));
    }
}

void shutdownTask() {
    // This client initiation pattern is only to be used here, with plans to eliminate this pattern
    // down the line.
    if (!haveClient()) {
        // This thread is technically killable, however we never actually expect it to be killed as
        // cryptd uses ReplicationCoordinatorNoOp.
        Client::initThread(getThreadName(), getGlobalServiceContext()->getService());
    }

    auto const client = Client::getCurrent();
    auto const serviceContext = client->getServiceContext();

    serviceContext->setKillAllOperations();

    // Shutdown watchdog before service entry point since it has a reference to the entry point
    shutdownIdleWatchdog(serviceContext);

    // Shutdown the TransportLayer so that new connections aren't accepted
    if (auto tl = serviceContext->getTransportLayerManager()) {
        LOGV2_OPTIONS(24226,
                      {logv2::LogComponent::kNetwork},
                      "shutdown: going to close listening sockets...");
        tl->shutdown();
    }

    serviceContext->setKillAllOperations();

    auto& lockFile = StorageEngineLockFile::get(serviceContext);
    if (lockFile) {
        lockFile->clearPidAndUnlock();
    }

    LOGV2(24227, "now exiting");
}

ExitCode initAndListen() {
    // This thread is technically killable, however we never actually expect it to be killed as
    // cryptd uses ReplicationCoordinatorNoOp.
    Client::initThread("initandlisten", getGlobalServiceContext()->getService());

    auto serviceContext = getGlobalServiceContext();
    {
        ProcessId pid = ProcessId::getCurrent();
        logv2::DynamicAttributes attrs;

        attrs.add("pid", pid.toNative());
        attrs.add("port", serverGlobalParams.port);
#ifndef _WIN32
        if (!serverGlobalParams.noUnixSocket) {
            // The unix socket is always appended to the back of our list of bind_ips.
            attrs.add("socketFile", serverGlobalParams.bind_ips.back());
        }
#endif
        const bool is32bit = sizeof(int*) == 4;
        attrs.add("architecture", is32bit ? "32-bit"_sd : "64-bit"_sd);
        std::string hostName = getHostNameCached();
        attrs.add("host", hostName);
        LOGV2(4615669, "MongoCryptD starting", attrs);
    }

    if (kDebugBuild)
        LOGV2(24228, "DEBUG build (which is slower)");

#ifdef _WIN32
    VersionInfoInterface::instance().logTargetMinOS();
#endif

    logProcessDetails(nullptr);

    createLockFile(serviceContext);

    startIdleWatchdog(serviceContext, mongoCryptDGlobalParams.idleShutdownTimeout);

    // $changeStream aggregations also check for the presence of a ReplicationCoordinator at parse
    // time, since change streams can only run on a replica set. If no such co-ordinator is present,
    // then $changeStream will assume that it is running on a standalone mongoD, and will return a
    // non-sequitur error to the user.
    repl::ReplicationCoordinator::set(
        serviceContext, std::make_unique<repl::ReplicationCoordinatorNoOp>(serviceContext));

    transport::ServiceExecutor::startupAll(serviceContext);

    if (auto status = serviceContext->getTransportLayerManager()->setup(); !status.isOK()) {
        LOGV2_ERROR(24233, "Failed to setup the transport layer", "error"_attr = redact(status));
        return ExitCode::netError;
    }

    if (auto status = serviceContext->getTransportLayerManager()->start(); !status.isOK()) {
        LOGV2_ERROR(24236, "Failed to start the transport layer", "error"_attr = redact(status));
        return ExitCode::netError;
    }

    serviceContext->notifyStorageStartupRecoveryComplete();

#ifndef _WIN32
    initialize_server_global_state::signalForkSuccess();
#else
    if (ntservice::shouldStartService()) {
        ntservice::reportStatus(SERVICE_RUNNING);
        LOGV2(24229, "Service running");
    }
#endif

    MONGO_IDLE_THREAD_BLOCK;
    return waitForShutdown();
}

#ifdef _WIN32
ExitCode initService() {
    return initAndListen();
}
#endif

MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    initialize_server_global_state::forkServerOrDie();
}

MONGO_INITIALIZER_WITH_PREREQUISITES(SetFeatureCompatibilityVersionLatest,
                                     ("EndStartupOptionStorage"))
(InitializerContext* context) {
    // Aggregations which include a $changeStream stage must read the current FCV during parsing. If
    // the FCV is not initialized, this will hit an invariant. We therefore initialize it here.
    // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);
}

int CryptDMain(int argc, char** argv) {
    registerShutdownTask(shutdownTask);

    setupSignalHandlers();
    runGlobalInitializersOrDie(std::vector<std::string>(argv, argv + argc));
    startSignalProcessingThread(LogFileStatus::kNoLogFileToRotate);

    auto serviceContextHolder = ServiceContext::make();
    setGlobalServiceContext(std::move(serviceContextHolder));
    auto serviceContext = getGlobalServiceContext();

    ShardingState::create(serviceContext);
    serviceContext->getService()->setServiceEntryPoint(std::make_unique<ServiceEntryPointCryptD>());

    auto tl = transport::TransportLayerManagerImpl::createWithConfig(
        &serverGlobalParams,
        serviceContext,
        false /* useEgressGRPC */,
        boost::none,
        boost::none,
        std::make_unique<ClientObserverCryptD>());
    serviceContext->setTransportLayerManager(std::move(tl));

#ifdef _WIN32
    ntservice::configureService(initService,
                                moe::startupOptionsParsed,
                                defaultServiceStrings,
                                std::vector<std::string>(),
                                std::vector<std::string>(argv, argv + argc));
#endif  // _WIN32

    cmdline_utils::censorArgvArray(argc, argv);

    if (!initialize_server_global_state::checkSocketPath())
        quickExit(ExitCode::fail);

    // Per SERVER-7434, startSignalProcessingThread must run after any forks (i.e.
    // initialize_server_global_state::forkServerOrDie) and before the creation of any other threads
    startSignalProcessingThread();

#ifdef _WIN32
    if (ntservice::shouldStartService()) {
        ntservice::startService();
        // exits directly and so never reaches here either.
    }
#endif  // _WIN32

    return static_cast<int>(initAndListen());
}

}  // namespace
}  // namespace mongo

#ifdef _WIN32
// In Windows, wmain() is an alternate entry point for main(), and receives the same parameters
// as main() but encoded in Windows Unicode (UTF-16); "wide" 16-bit wchar_t characters.  The
// WindowsCommandLine object converts these wide character strings to a UTF-8 coded equivalent
// and makes them available through argv().  This enables CrytpDMain()
// to process UTF-8 encoded arguments without regard to platform.
int wmain(int argc, wchar_t** argvW) {
    mongo::WindowsCommandLine wcl(argc, argvW);
    int exitCode = mongo::CryptDMain(argc, wcl.argv());
    mongo::quickExit(exitCode);
}
#else
int main(int argc, char** argv) {
    int exitCode = mongo::CryptDMain(argc, argv);
    mongo::quickExit(exitCode);
}
#endif
