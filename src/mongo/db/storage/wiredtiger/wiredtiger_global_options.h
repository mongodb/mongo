// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/options_parser/environment.h"

#include <cstddef>
#include <string>

namespace mongo {

class WiredTigerGlobalOptions {
public:
    WiredTigerGlobalOptions()
        : cacheSizeGB(0),
          cacheSizePct(0),
          statisticsLogDelaySecs(0),
          zstdCompressorLevel(0),
          directoryForIndexes(false),
          maxCacheOverflowFileSizeGBDeprecated(0),
          liveRestoreThreads(0),
          liveRestoreReadSizeMB(0),
          useIndexPrefixCompression(false),
          statisticsSetting("fast") {};

    Status store(const optionenvironment::Environment& params);

    double cacheSizeGB;
    double cacheSizePct;
    size_t statisticsLogDelaySecs;
    int32_t sessionMax{0};
    int32_t reservedSessionMax{0};
    double evictionDirtyTargetGB{0};
    double evictionDirtyTriggerGB{0};
    double evictionUpdatesTriggerGB{0};
    std::string journalCompressor;
    int zstdCompressorLevel;
    // NEEDS REPLACEMENT: this should really be a storage option not a WT option.
    [[MONGO_MOD_NEEDS_REPLACEMENT]] bool directoryForIndexes;
    double maxCacheOverflowFileSizeGBDeprecated;
    std::string engineConfig;
    std::string liveRestoreSource;
    int liveRestoreThreads;
    double liveRestoreReadSizeMB;

    std::string collectionBlockCompressor;
    bool useIndexPrefixCompression;
    std::string collectionConfig;
    std::string indexConfig;
    std::string statisticsSetting;

    static Status validateWiredTigerCompressor(const std::string&);
    static Status validateSpillWiredTigerCompressor(const std::string&,
                                                    const boost::optional<TenantId>&);
    static Status validateWiredTigerLiveRestoreReadSizeMB(int);
    static Status validateStatisticsSetting(const std::string&);

    /**
     * Returns current history file size limit in MB.
     * Always returns 0 for unbounded.
     */
    std::size_t getMaxHistoryFileSizeMB() const {
        return 0;
    }
};

// NEEDS REPLACEMENT: this should really be a storage option not a WT option.
[[MONGO_MOD_NEEDS_REPLACEMENT]] extern WiredTigerGlobalOptions wiredTigerGlobalOptions;

}  // namespace mongo
