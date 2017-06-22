/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
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
#include "mongo/db/ftdc/ftdc_system_stats.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace {

const auto getFTDCController = ServiceContext::declareDecoration<std::unique_ptr<FTDCController>>();

FTDCController* getGlobalFTDCController() {
    if (!hasGlobalServiceContext()) {
        return nullptr;
    }

    return getFTDCController(getGlobalServiceContext()).get();
}

AtomicBool localEnabledFlag(FTDCConfig::kEnabledDefault);

class ExportedFTDCEnabledParameter
    : public ExportedServerParameter<bool, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedFTDCEnabledParameter()
        : ExportedServerParameter<bool, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(),
              "diagnosticDataCollectionEnabled",
              &localEnabledFlag) {}

    virtual Status validate(const bool& potentialNewValue) {
        auto controller = getGlobalFTDCController();
        if (controller) {
            return controller->setEnabled(potentialNewValue);
        }

        return Status::OK();
    }

} exportedFTDCEnabledParameter;

AtomicInt32 localPeriodMillis(FTDCConfig::kPeriodMillisDefault);

class ExportedFTDCPeriodParameter
    : public ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedFTDCPeriodParameter()
        : ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(),
              "diagnosticDataCollectionPeriodMillis",
              &localPeriodMillis) {}

    virtual Status validate(const std::int32_t& potentialNewValue) {
        if (potentialNewValue < 100) {
            return Status(
                ErrorCodes::BadValue,
                "diagnosticDataCollectionPeriodMillis must be greater than or equal to 100ms");
        }

        auto controller = getGlobalFTDCController();
        if (controller) {
            controller->setPeriod(Milliseconds(potentialNewValue));
        }

        return Status::OK();
    }

} exportedFTDCPeriodParameter;

// Scale the values down since are defaults are in bytes, but the user interface is MB
AtomicInt32 localMaxDirectorySizeMB(FTDCConfig::kMaxDirectorySizeBytesDefault / (1024 * 1024));

AtomicInt32 localMaxFileSizeMB(FTDCConfig::kMaxFileSizeBytesDefault / (1024 * 1024));

class ExportedFTDCDirectorySizeParameter
    : public ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedFTDCDirectorySizeParameter()
        : ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(),
              "diagnosticDataCollectionDirectorySizeMB",
              &localMaxDirectorySizeMB) {}

    virtual Status validate(const std::int32_t& potentialNewValue) {
        if (potentialNewValue < 10) {
            return Status(
                ErrorCodes::BadValue,
                "diagnosticDataCollectionDirectorySizeMB must be greater than or equal to 10");
        }

        if (potentialNewValue < localMaxFileSizeMB.load()) {
            return Status(
                ErrorCodes::BadValue,
                str::stream()
                    << "diagnosticDataCollectionDirectorySizeMB must be greater than or equal to '"
                    << localMaxFileSizeMB.load()
                    << "' which is the current value of diagnosticDataCollectionFileSizeMB.");
        }

        auto controller = getGlobalFTDCController();
        if (controller) {
            controller->setMaxDirectorySizeBytes(potentialNewValue * 1024 * 1024);
        }

        return Status::OK();
    }

} exportedFTDCDirectorySizeParameter;

class ExportedFTDCFileSizeParameter
    : public ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedFTDCFileSizeParameter()
        : ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(),
              "diagnosticDataCollectionFileSizeMB",
              &localMaxFileSizeMB) {}

    virtual Status validate(const std::int32_t& potentialNewValue) {
        if (potentialNewValue < 1) {
            return Status(ErrorCodes::BadValue,
                          "diagnosticDataCollectionFileSizeMB must be greater than or equal to 1");
        }

        if (potentialNewValue > localMaxDirectorySizeMB.load()) {
            return Status(
                ErrorCodes::BadValue,
                str::stream()
                    << "diagnosticDataCollectionFileSizeMB must be less than or equal to '"
                    << localMaxDirectorySizeMB.load()
                    << "' which is the current value of diagnosticDataCollectionDirectorySizeMB.");
        }

        auto controller = getGlobalFTDCController();
        if (controller) {
            controller->setMaxFileSizeBytes(potentialNewValue * 1024 * 1024);
        }

        return Status::OK();
    }

} exportedFTDCFileSizeParameter;

AtomicInt32 localMaxSamplesPerArchiveMetricChunk(
    FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault);

class ExportedFTDCArchiveChunkSizeParameter
    : public ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedFTDCArchiveChunkSizeParameter()
        : ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(),
              "diagnosticDataCollectionSamplesPerChunk",
              &localMaxSamplesPerArchiveMetricChunk) {}

    virtual Status validate(const std::int32_t& potentialNewValue) {
        if (potentialNewValue < 2) {
            return Status(
                ErrorCodes::BadValue,
                "diagnosticDataCollectionSamplesPerChunk must be greater than or equal to 2");
        }

        auto controller = getGlobalFTDCController();
        if (controller) {
            controller->setMaxSamplesPerArchiveMetricChunk(potentialNewValue);
        }

        return Status::OK();
    }

} exportedFTDCArchiveChunkSizeParameter;

AtomicInt32 localMaxSamplesPerInterimMetricChunk(
    FTDCConfig::kMaxSamplesPerInterimMetricChunkDefault);

class ExportedFTDCInterimChunkSizeParameter
    : public ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime> {
public:
    ExportedFTDCInterimChunkSizeParameter()
        : ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>(
              ServerParameterSet::getGlobal(),
              "diagnosticDataCollectionSamplesPerInterimUpdate",
              &localMaxSamplesPerInterimMetricChunk) {}

    virtual Status validate(const std::int32_t& potentialNewValue) {
        if (potentialNewValue < 2) {
            return Status(ErrorCodes::BadValue,
                          "diagnosticDataCollectionSamplesPerInterimUpdate must be greater than or "
                          "equal to 2");
        }

        auto controller = getGlobalFTDCController();
        if (controller) {
            controller->setMaxSamplesPerInterimMetricChunk(potentialNewValue);
        }

        return Status::OK();
    }

} exportedFTDCInterimChunkSizeParameter;
}  // namespace

FTDCSimpleInternalCommandCollector::FTDCSimpleInternalCommandCollector(StringData command,
                                                                       StringData name,
                                                                       StringData ns,
                                                                       BSONObj cmdObj)
    : _name(name.toString()), _request(OpMsgRequest::fromDBAndBody(ns, std::move(cmdObj))) {
    invariant(command == _request.getCommandName());
    invariant(Command::findCommand(command));  // Fail early if it doesn't exist.
}

void FTDCSimpleInternalCommandCollector::collect(OperationContext* opCtx, BSONObjBuilder& builder) {
    auto result = Command::runCommandDirectly(opCtx, _request);
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
    config.period = Milliseconds(localPeriodMillis.load());
    // Only enable FTDC if our caller says to enable FTDC, MongoS may not have a valid path to write
    // files to so update the diagnosticDataCollectionEnabled set parameter to reflect that.
    localEnabledFlag.store(startupMode == FTDCStartMode::kStart && localEnabledFlag.load());
    config.enabled = localEnabledFlag.load();
    config.maxFileSizeBytes = localMaxFileSizeMB.load() * 1024 * 1024;
    config.maxDirectorySizeBytes = localMaxDirectorySizeMB.load() * 1024 * 1024;
    config.maxSamplesPerArchiveMetricChunk = localMaxSamplesPerArchiveMetricChunk.load();
    config.maxSamplesPerInterimMetricChunk = localMaxSamplesPerInterimMetricChunk.load();

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
    // TODO: do we need to enable "sharding" on MongoS?
    controller->addPeriodicCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "serverStatus",
        "serverStatus",
        "",
        BSON("serverStatus" << 1 << "tcMalloc" << true << "sharding" << false)));

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
