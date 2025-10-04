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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/op_msg.h"

#include <cstdint>
#include <functional>
#include <string>

#include <boost/filesystem/path.hpp>

namespace mongo {

/**
 * Function that allows FTDC server components to register their own collectors as needed.
 */
using RegisterCollectorsFunction = std::function<void(FTDCController*)>;

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
    FTDCSimpleInternalCommandCollector(StringData command,
                                       StringData name,
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
    AtomicWord<bool> enabled;
    AtomicWord<int> periodMillis;
    AtomicWord<int> metadataCaptureFrequency;
    AtomicWord<int> sampleTimeoutMillis;
    AtomicWord<int> minThreads;
    AtomicWord<int> maxThreads;

    AtomicWord<int> maxDirectorySizeMB;
    AtomicWord<int> maxFileSizeMB;
    AtomicWord<int> maxSamplesPerArchiveMetricChunk;
    AtomicWord<int> maxSamplesPerInterimMetricChunk;

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
