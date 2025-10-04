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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <cstdint>
#include <map>

namespace mongo {

/**
 * Container for server-wide statistics on writeConcern levels used by operations.
 */
class MONGO_MOD_PUB ServerWriteConcernMetrics {
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

    mutable stdx::mutex _mutex;
    WriteConcernMetricsForOperationType _insertMetrics;
    WriteConcernMetricsForOperationType _updateMetrics;
    WriteConcernMetricsForOperationType _deleteMetrics;
};

}  // namespace mongo
