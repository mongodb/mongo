// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <boost/filesystem/path.hpp>

namespace mongo {

/**
 * Function that allows FTDC server components to register their own collectors as needed.
 */
using RegisterCollectorsFunction = std::function<void(ServiceContext*, FTDCController*)>;

/**
 * An enum that decides whether FTDC will startup as part of startup or if its deferred to later.
 */
enum class FTDCStartMode {

    /**
     * Skip starting FTDC since it missing a file storage location.
     */
    kSkipStart,

    /**
     * Start FTDC because it has a path to store files.
     */
    kStart,
};

/**
 * Start Full Time Data Capture.
 * Starts 1 thread.
 *
 * See MongoD and MongoS specific functions.
 */
void startFTDC(ServiceContext* serviceContext,
               boost::filesystem::path& path,
               FTDCStartMode startupMode,
               std::vector<RegisterCollectorsFunction> registerCollectorsFns);

/**
 * Stop Full Time Data Capture
 *
 * See MongoD and MongoS specific functions.
 */
void stopFTDC();

/**
 * Register collectors wanted by all server roles.
 */
void registerServerCollectors(FTDCController* controller);

/**
 * A simple FTDC Collector that runs Commands.
 */
class FTDCSimpleInternalCommandCollector : public FTDCCollectorInterface {
public:
    FTDCSimpleInternalCommandCollector(std::string_view command,
                                       std::string_view name,
                                       const DatabaseName& db,
                                       BSONObj cmdObj);

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) override;
    std::string name() const override;

private:
    std::string _name;
    const OpMsgRequest _request;
};

/**
 * FTDC startup parameters.
 *
 * Used to provide default values from FTDCConfig to the FTDC set parameters.
 */
struct FTDCStartupParams {
    Atomic<bool> enabled;
    Atomic<int> periodMillis;
    Atomic<int> metadataCaptureFrequency;
    Atomic<int> sampleTimeoutMillis;
    Atomic<int> minThreads;
    Atomic<int> maxThreads;

    Atomic<int> maxDirectorySizeMB;
    Atomic<int> maxFileSizeMB;
    Atomic<int> maxSamplesPerArchiveMetricChunk;
    Atomic<int> maxSamplesPerInterimMetricChunk;

    FTDCStartupParams()
        : enabled(FTDCConfig::kEnabledDefault),
          periodMillis(FTDCConfig::kPeriodMillisDefault),
          metadataCaptureFrequency(FTDCConfig::kMetadataCaptureFrequencyDefault),
          sampleTimeoutMillis(FTDCConfig::kSampleTimeoutMillisDefault),
          minThreads(FTDCConfig::kMinThreadsDefault),
          maxThreads(FTDCConfig::kMaxThreadsDefault),
          // Scale the values down since are defaults are in bytes, but the user interface is MB
          maxDirectorySizeMB(FTDCConfig::kMaxDirectorySizeBytesDefault / (1024 * 1024)),
          maxFileSizeMB(FTDCConfig::kMaxFileSizeBytesDefault / (1024 * 1024)),
          maxSamplesPerArchiveMetricChunk(FTDCConfig::kMaxSamplesPerArchiveMetricChunkDefault),
          maxSamplesPerInterimMetricChunk(FTDCConfig::kMaxSamplesPerInterimMetricChunkDefault) {}
};

extern FTDCStartupParams ftdcStartupParams;

/**
 * Server Parameter callbacks
 */
Status onUpdateFTDCEnabled(bool value);
Status onUpdateFTDCPeriod(std::int32_t value);
Status onUpdateFTDCMetadataCaptureFrequency(std::int32_t value);
Status onUpdateFTDCDirectorySize(std::int32_t value);
Status onUpdateFTDCFileSize(std::int32_t value);
Status onUpdateFTDCSamplesPerChunk(std::int32_t value);
Status onUpdateFTDCPerInterimUpdate(std::int32_t value);
Status onUpdateFTDCSampleTimeout(std::int32_t value);
Status onUpdateFTDCMinThreads(std::int32_t value);
Status onUpdateFTDCMaxThreads(std::int32_t value);
Status validateSampleTimeoutMillis(std::int32_t, const boost::optional<TenantId>&);

/**
 * Server Parameter accessors
 */
boost::filesystem::path getFTDCDirectoryPathParameter();

}  // namespace mongo
