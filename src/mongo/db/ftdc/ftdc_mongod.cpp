/**
 * Copyright (C) 2015 MongoDB Inc.
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

#include <boost/filesystem.hpp>
#include <fstream>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_system_stats.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"

namespace mongo {

namespace {

const auto getFTDCController = ServiceContext::declareDecoration<std::unique_ptr<FTDCController>>();

FTDCController* getGlobalFTDCController() {
    if (!hasGlobalServiceContext()) {
        return nullptr;
    }

    return getFTDCController(getGlobalServiceContext()).get();
}

std::atomic<bool> localEnabledFlag(FTDCConfig::kEnabledDefault);  // NOLINT

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
            controller->setEnabled(potentialNewValue);
        }

        return Status::OK();
    }

} exportedFTDCEnabledParameter;

std::atomic<std::int32_t> localPeriodMillis(FTDCConfig::kPeriodMillisDefault);  // NOLINT

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
std::atomic<std::int32_t> localMaxDirectorySizeMB(  // NOLINT
    FTDCConfig::kMaxDirectorySizeBytesDefault / (1024 * 1024));

std::atomic<std::int32_t> localMaxFileSizeMB(FTDCConfig::kMaxFileSizeBytesDefault /  // NOLINT
                                             (1024 * 1024));

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

        if (potentialNewValue < localMaxFileSizeMB) {
            return Status(
                ErrorCodes::BadValue,
                str::stream()
                    << "diagnosticDataCollectionDirectorySizeMB must be greater than or equal to '"
                    << localMaxFileSizeMB
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

        if (potentialNewValue > localMaxDirectorySizeMB) {
            return Status(
                ErrorCodes::BadValue,
                str::stream()
                    << "diagnosticDataCollectionFileSizeMB must be less than or equal to '"
                    << localMaxDirectorySizeMB
                    << "' which is the current value of diagnosticDataCollectionDirectorySizeMB.");
        }

        auto controller = getGlobalFTDCController();
        if (controller) {
            controller->setMaxFileSizeBytes(potentialNewValue * 1024 * 1024);
        }

        return Status::OK();
    }

} exportedFTDCFileSizeParameter;

std::atomic<std::int32_t> localMaxSamplesPerArchiveMetricChunk(  // NOLINT
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

std::atomic<std::int32_t> localMaxSamplesPerInterimMetricChunk(  // NOLINT
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

class FTDCSimpleInternalCommandCollector final : public FTDCCollectorInterface {
public:
    FTDCSimpleInternalCommandCollector(StringData command,
                                       StringData name,
                                       StringData ns,
                                       BSONObj cmdObj)
        : _name(name.toString()), _ns(ns.toString()), _cmdObj(std::move(cmdObj)) {
        _command = Command::findCommand(command);
        invariant(_command);
    }

    void collect(OperationContext* txn, BSONObjBuilder& builder) override {
        std::string errmsg;

        bool ret = _command->run(txn, _ns, _cmdObj, 0, errmsg, builder);

        // Some commands return errmsgs when they return false (collstats)
        // Some commands return bson objs when they return false (replGetStatus)
        // We append the status as needed to ensure readers of the collected data can check the
        // status of any individual command.
        _command->appendCommandStatus(builder, ret, errmsg);
    }

    std::string name() const override {
        return _name;
    }

private:
    std::string _name;
    std::string _ns;
    BSONObj _cmdObj;

    // Not owned
    Command* _command;
};

}  // namespace

// Register the FTDC system
// Note: This must be run before the server parameters are parsed during startup
// so that the FTDCController is initialized.
//
void startFTDC() {
    boost::filesystem::path dir(storageGlobalParams.dbpath);
    dir /= "diagnostic.data";


    FTDCConfig config;
    config.period = Milliseconds(localPeriodMillis.load());
    config.enabled = localEnabledFlag;
    config.maxFileSizeBytes = localMaxFileSizeMB * 1024 * 1024;
    config.maxDirectorySizeBytes = localMaxDirectorySizeMB * 1024 * 1024;
    config.maxSamplesPerArchiveMetricChunk = localMaxSamplesPerArchiveMetricChunk;
    config.maxSamplesPerInterimMetricChunk = localMaxSamplesPerInterimMetricChunk;

    auto controller = stdx::make_unique<FTDCController>(dir, config);

    // Install periodic collectors
    // These are collected on the period interval in FTDCConfig.

    // CmdServerStatus
    controller->addPeriodicCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
        "serverStatus", "serverStatus", "", BSON("serverStatus" << 1 << "tcMalloc" << true)));

    // These metrics are only collected if replication is enabled
    if (repl::getGlobalReplicationCoordinator()->getReplicationMode() !=
        repl::ReplicationCoordinator::modeNone) {
        // CmdReplSetGetStatus
        controller->addPeriodicCollector(stdx::make_unique<FTDCSimpleInternalCommandCollector>(
            "replSetGetStatus", "replSetGetStatus", "", BSON("replSetGetStatus" << 1)));

        // CollectionStats
        controller->addPeriodicCollector(
            stdx::make_unique<FTDCSimpleInternalCommandCollector>("collStats",
                                                                  "local.oplog.rs.stats",
                                                                  "local",
                                                                  BSON("collStats"
                                                                       << "oplog.rs")));
    }

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

}  // namespace mongo
