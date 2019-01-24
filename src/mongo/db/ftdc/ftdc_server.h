
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

#include <string>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/functional.h"

namespace mongo {

/**
 * Function that allows FTDC server components to register their own collectors as needed.
 */
using RegisterCollectorsFunction = stdx::function<void(FTDCController*)>;

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
 * Start Full Time Data Capture
 * Starts 1 thread.
 *
 * See MongoD and MongoS specific functions.
 */
void startFTDC(boost::filesystem::path& path,
               FTDCStartMode startupMode,
               RegisterCollectorsFunction registerCollectors);

/**
 * Stop Full Time Data Capture
 *
 * See MongoD and MongoS specific functions.
 */
void stopFTDC();

/**
 * A simple FTDC Collector that runs Commands.
 */
class FTDCSimpleInternalCommandCollector : public FTDCCollectorInterface {
public:
    FTDCSimpleInternalCommandCollector(StringData command,
                                       StringData name,
                                       StringData ns,
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

    AtomicWord<int> maxDirectorySizeMB;
    AtomicWord<int> maxFileSizeMB;
    AtomicWord<int> maxSamplesPerArchiveMetricChunk;
    AtomicWord<int> maxSamplesPerInterimMetricChunk;

    FTDCStartupParams()
        : enabled(FTDCConfig::kEnabledDefault),
          periodMillis(FTDCConfig::kPeriodMillisDefault),
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
Status onUpdateFTDCEnabled(const bool value);
Status onUpdateFTDCPeriod(const std::int32_t value);
Status onUpdateFTDCDirectorySize(const std::int32_t value);
Status onUpdateFTDCFileSize(const std::int32_t value);
Status onUpdateFTDCSamplesPerChunk(const std::int32_t value);
Status onUpdateFTDCPerInterimUpdate(const std::int32_t value);

/**
 * Server Parameter accessors
 */
boost::filesystem::path getFTDCDirectoryPathParameter();

}  // namespace mongo
