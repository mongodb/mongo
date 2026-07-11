// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>

namespace mongo {

/**
 * Container for server-wide statistics on writeConcern levels used by operations.
 */
class [[MONGO_MOD_PUBLIC]] ServerWriteConcernMetrics {
    ServerWriteConcernMetrics(const ServerWriteConcernMetrics&) = delete;
    ServerWriteConcernMetrics& operator=(const ServerWriteConcernMetrics&) = delete;

public:
    ServerWriteConcernMetrics() = default;

    static ServerWriteConcernMetrics* get(ServiceContext* service);
    static ServerWriteConcernMetrics* get(OperationContext* opCtx);

    /**
     * Updates the insert metrics 'numInserts' times according to the 'w' value of
     * 'writeConcernOptions'.
     */
    void recordWriteConcernForInserts(const WriteConcernOptions& writeConcernOptions,
                                      size_t numInserts);

    /**
     * Updates the insert metrics according to the 'w' value of 'writeConcernOptions'.
     */
    void recordWriteConcernForInsert(const WriteConcernOptions& writeConcernOptions) {
        recordWriteConcernForInserts(writeConcernOptions, 1);
    }

    /**
     * Updates the update metrics according to the 'w' value of 'writeConcernOptions'.
     */
    void recordWriteConcernForUpdate(const WriteConcernOptions& writeConcernOptions);

    /**
     * Updates the delete metrics according to the 'w' value of 'writeConcernOptions'.
     */
    void recordWriteConcernForDelete(const WriteConcernOptions& writeConcernOptions);

    BSONObj toBSON() const;

private:
    struct WriteConcernCounters {
        WriteConcernCounters() = default;

        WriteConcernCounters(bool exportWTag) : exportWTag(exportWTag) {}

        // Count of operations with writeConcern w:"majority".
        std::uint64_t wMajorityCount = 0;

        // Counts of operations with writeConcern w:<num>.
        std::map<int, std::uint64_t> wNumCounts;

        // Set to true to include "wTag" section when exporting to BSON.
        bool exportWTag = true;

        // Counts of operations with writeConcern w:"tag".
        StringMap<std::uint64_t> wTagCounts;

        /**
         * Updates counters for the 'w' value of 'writeConcernOptions'.
         */
        void recordWriteConcern(const WriteConcernOptions& writeConcernOptions, size_t numOps);

        void toBSON(BSONObjBuilder* builder) const;
    };

    struct WriteConcernMetricsForOperationType {
        /**
         * Updates the corresponding WC counters for the 'w' value of 'writeConcernOptions'.
         */
        void recordWriteConcern(const WriteConcernOptions& writeConcernOptions, size_t numOps = 1);

        void toBSON(BSONObjBuilder* builder) const;

        // Counts of operations with writeConcern with 'w' value explicitly set by client.
        WriteConcernCounters explicitWC;

        // Counts of operations used cluster-wide writeConcern.
        WriteConcernCounters cWWC;

        // Counts of operations used implicit default writeConcern.
        WriteConcernCounters implicitDefaultWC = WriteConcernCounters(false);

        // Count of operations without an explicit writeConcern with "w" value.
        std::uint64_t notExplicitWCount = 0;

        // Count of operations with explicit write concern { "w" : majority, "j" : false }
        // overridden to "j" : true.
        std::uint64_t majorityJFalseOverriddenCount = 0;
    };

    mutable std::mutex _mutex;
    WriteConcernMetricsForOperationType _insertMetrics;
    WriteConcernMetricsForOperationType _updateMetrics;
    WriteConcernMetricsForOperationType _deleteMetrics;
};

}  // namespace mongo
