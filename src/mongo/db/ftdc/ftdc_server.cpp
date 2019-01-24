
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

#include "mongo/db/ftdc/ftdc_server.h"

#include <boost/filesystem.hpp>
#include <fstream>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_server_gen.h"
#include "mongo/db/ftdc/ftdc_system_stats.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {

namespace {

const auto getFTDCController = ServiceContext::declareDecoration<std::unique_ptr<FTDCController>>();

FTDCController* getGlobalFTDCController() {
    if (!hasGlobalServiceContext()) {
        return nullptr;
    }

    return getFTDCController(getGlobalServiceContext()).get();
}

/**
 * Expose diagnosticDataCollectionDirectoryPath set parameter to specify the MongoD and MongoS FTDC
 * path.
 */
synchronized_value<boost::filesystem::path> ftdcDirectoryPathParameter;

}  // namespace

FTDCStartupParams ftdcStartupParams;

void DiagnosticDataCollectionDirectoryPathServerParameter::append(OperationContext* opCtx,
                                                                  BSONObjBuilder& b,
                                                                  const std::string& name) {
    b.append(name, ftdcDirectoryPathParameter->generic_string());
}

Status DiagnosticDataCollectionDirectoryPathServerParameter::setFromString(const std::string& str) {
    if (hasGlobalServiceContext()) {
        FTDCController* controller = FTDCController::get(getGlobalServiceContext());
        if (controller) {
            Status s = controller->setDirectory(str);
            if (!s.isOK()) {
                return s;
            }
        }
    }

    ftdcDirectoryPathParameter = str;

    return Status::OK();
}

boost::filesystem::path getFTDCDirectoryPathParameter() {
    return ftdcDirectoryPathParameter.get();
}

Status onUpdateFTDCEnabled(const bool value) {
    auto controller = getGlobalFTDCController();
    if (controller) {
        return controller->setEnabled(value);
    }

    return Status::OK();
}

Status onUpdateFTDCPeriod(const std::int32_t potentialNewValue) {
    auto controller = getGlobalFTDCController();
    if (controller) {
        controller->setPeriod(Milliseconds(potentialNewValue));
    }

    return Status::OK();
}

Status onUpdateFTDCDirectorySize(const std::int32_t potentialNewValue) {
    if (potentialNewValue < ftdcStartupParams.maxFileSizeMB.load()) {
        return Status(
            ErrorCodes::BadValue,
            str::stream()
                << "diagnosticDataCollectionDirectorySizeMB must be greater than or equal to '"
                << ftdcStartupParams.maxFileSizeMB.load()
                << "' which is the current value of diagnosticDataCollectionFileSizeMB.");
    }

    auto controller = getGlobalFTDCController();
    if (controller) {
        controller->setMaxDirectorySizeBytes(potentialNewValue * 1024 * 1024);
    }

    return Status::OK();
}

Status onUpdateFTDCFileSize(const std::int32_t potentialNewValue) {
    if (potentialNewValue > ftdcStartupParams.maxDirectorySizeMB.load()) {
        return Status(
            ErrorCodes::BadValue,
            str::stream()
                << "diagnosticDataCollectionFileSizeMB must be less than or equal to '"
                << ftdcStartupParams.maxDirectorySizeMB.load()
                << "' which is the current value of diagnosticDataCollectionDirectorySizeMB.");
    }

    auto controller = getGlobalFTDCController();
    if (controller) {
        controller->setMaxFileSizeBytes(potentialNewValue * 1024 * 1024);
    }

    return Status::OK();
}

Status onUpdateFTDCSamplesPerChunk(const std::int32_t potentialNewValue) {
    auto controller = getGlobalFTDCController();
    if (controller) {
        controller->setMaxSamplesPerArchiveMetricChunk(potentialNewValue);
    }

    return Status::OK();
}

Status onUpdateFTDCPerInterimUpdate(const std::int32_t potentialNewValue) {
    auto controller = getGlobalFTDCController();
    if (controller) {
        controller->setMaxSamplesPerInterimMetricChunk(potentialNewValue);
    }

    return Status::OK();
}

FTDCSimpleInternalCommandCollector::FTDCSimpleInternalCommandCollector(StringData command,
                                                                       StringData name,
                                                                       StringData ns,
                                                                       BSONObj cmdObj)
    : _name(name.toString()), _request(OpMsgRequest::fromDBAndBody(ns, std::move(cmdObj))) {
    invariant(command == _request.getCommandName());
    invariant(CommandHelpers::findCommand(command));  // Fail early if it doesn't exist.
}

void FTDCSimpleInternalCommandCollector::collect(OperationContext* opCtx, BSONObjBuilder& builder) {
    auto result = CommandHelpers::runCommandDirectly(opCtx, _request);
    builder.appendElements(result);
}

std::string FTDCSimpleInternalCommandCollector::name() const {
    return _name;
}

// Register the FTDC system
// Note: This must be run before the server parameters are parsed during startup
// so that the FTDCController is initialized.
//
void startFTDC(boost::filesystem::path& path,
               FTDCStartMode startupMode,
               RegisterCollectorsFunction registerCollectors) {
    FTDCConfig config;
    config.period = Milliseconds(ftdcStartupParams.periodMillis.load());
    // Only enable FTDC if our caller says to enable FTDC, MongoS may not have a valid path to write
    // files to so update the diagnosticDataCollectionEnabled set parameter to reflect that.
    ftdcStartupParams.enabled.store(startupMode == FTDCStartMode::kStart &&
                                    ftdcStartupParams.enabled.load());
    config.enabled = ftdcStartupParams.enabled.load();
    config.maxFileSizeBytes = ftdcStartupParams.maxFileSizeMB.load() * 1024 * 1024;
    config.maxDirectorySizeBytes = ftdcStartupParams.maxDirectorySizeMB.load() * 1024 * 1024;
    config.maxSamplesPerArchiveMetricChunk =
        ftdcStartupParams.maxSamplesPerArchiveMetricChunk.load();
    config.maxSamplesPerInterimMetricChunk =
        ftdcStartupParams.maxSamplesPerInterimMetricChunk.load();

    ftdcDirectoryPathParameter = path;

    auto controller = stdx::make_unique<FTDCController>(path, config);

    // Install periodic collectors
    // These are collected on the period interval in FTDCConfig.
    // NOTE: For each command here, there must be an equivalent privilege check in
    // GetDiagnosticDataCommand

    // CmdServerStatus
    // The "sharding" section is filtered out because at this time it only consists of strings in
    // migration status. This section triggers too many schema changes in the serverStatus which
    // hurt ftdc compression efficiency, because its output varies depending on the list of active
    // migrations.
    // "timing" is filtered out because it triggers frequent schema changes.
    // TODO: do we need to enable "sharding" on MongoS?
    controller->addPeriodicCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "serverStatus",
        "serverStatus",
        "",
        BSON("serverStatus" << 1 << "tcMalloc" << true << "sharding" << false << "timing"
                            << false)));

    registerCollectors(controller.get());

    // Install System Metric Collector as a periodic collector
    installSystemMetricsCollector(controller.get());

    // Install file rotation collectors
    // These are collected on each file rotation.

    // CmdBuildInfo
    controller->addOnRotateCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "buildInfo", "buildInfo", "", BSON("buildInfo" << 1)));

    // CmdGetCmdLineOpts
    controller->addOnRotateCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "getCmdLineOpts", "getCmdLineOpts", "", BSON("getCmdLineOpts" << 1)));

    // HostInfoCmd
    controller->addOnRotateCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "hostInfo", "hostInfo", "", BSON("hostInfo" << 1)));

    // Install the new controller
    auto& staticFTDC = getFTDCController(getGlobalServiceContext());

    staticFTDC = std::move(controller);

    staticFTDC->start();
}

void stopFTDC() {
    auto controller = getGlobalFTDCController();

    if (controller) {
        controller->stop();
    }
}

FTDCController* FTDCController::get(ServiceContext* serviceContext) {
    return getFTDCController(serviceContext).get();
}

}  // namespace mongo
