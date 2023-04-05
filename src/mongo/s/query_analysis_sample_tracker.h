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

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/uuid.h"

#include <boost/optional.hpp>
#include <map>
#include <vector>

namespace mongo {
namespace analyze_shard_key {

/**
 * Maintains read and write counters of queries being sampled for shard key analysis. This includes
 * server-wide counters and per-collection counters for the collections that have query sampling
 * enabled. Instances of this object on mongod will also count the number of bytes being
 * written to the sample collection.
 */
class QueryAnalysisSampleTracker {
public:
    class CollectionSampleTracker {
    public:
        CollectionSampleTracker(const NamespaceString& nss,
                                const UUID& collUuid,
                                double sampleRate,
                                const Date_t& startTime)
            : _nss(nss), _collUuid(collUuid), _sampleRate(sampleRate), _startTime(startTime){};

        NamespaceString getNs() const {
            return _nss;
        }

        UUID getCollUuid() const {
            return _collUuid;
        }

        void setSampleRate(double sampleRate) {
            _sampleRate = sampleRate;
        }

        void setStartTime(Date_t startTime) {
            _startTime = startTime;
        }

        /**
         * Increments the read counter and adds <size> to the read bytes counter.
         */
        void incrementReads(boost::optional<int64_t> size = boost::none) {
            ++_sampledReadsCount;
            if (size) {
                _sampledReadsBytes += *size;
            }
        }

        /**
         * Increments the write counter and adds <size> to the write bytes counter.
         */
        void incrementWrites(boost::optional<int64_t> size = boost::none) {
            ++_sampledWritesCount;
            if (size) {
                _sampledWritesBytes += *size;
            }
        }

        BSONObj reportForCurrentOp() const;

    private:
        NamespaceString _nss;
        UUID _collUuid;
        int64_t _sampledReadsCount = 0;
        int64_t _sampledReadsBytes = 0;
        int64_t _sampledWritesCount = 0;
        int64_t _sampledWritesBytes = 0;
        double _sampleRate;
        Date_t _startTime;
    };

    QueryAnalysisSampleTracker() {}

    /**
     * Returns a reference to the service-wide QueryAnalysisSampleTracker instance.
     */
    static QueryAnalysisSampleTracker& get(OperationContext* opCtx);
    static QueryAnalysisSampleTracker& get(ServiceContext* serviceContext);

    void refreshConfigurations(
        const std::vector<CollectionQueryAnalyzerConfiguration>& configurations);

    /**
     * Retrieves the collection's sample counters given the namespace string and the collection
     * UUID. If the collection's sample counters do not exist, new counters are created for the
     * collection and returned.
     */
    void incrementReads(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const boost::optional<UUID>& collUuid = boost::none,
                        boost::optional<int64_t> size = boost::none);
    void incrementWrites(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const boost::optional<UUID>& collUuid = boost::none,
                         boost::optional<int64_t> size = boost::none);

    /**
     * Reports sample counters for each collection, inserting one BSONObj per collection.
     */
    void reportForCurrentOp(std::vector<BSONObj>* ops) const;

    /**
     * Reports number of queries sampled over the lifetime of the server.
     */
    BSONObj reportForServerStatus() const;

private:
    std::shared_ptr<CollectionSampleTracker> _getOrCreateCollectionSampleTracker(
        WithLock,
        OperationContext* opCtx,
        const NamespaceString& nss,
        const boost::optional<UUID>& collUuid);

    mutable Mutex _mutex = MONGO_MAKE_LATCH("QueryAnalysisSampleTracker::_mutex");

    int64_t _totalSampledReadsCount = 0;
    int64_t _totalSampledWritesCount = 0;
    int64_t _totalSampledReadsBytes = 0;
    int64_t _totalSampledWritesBytes = 0;

    // Per-collection sample trackers. When sampling for a collection is turned off, its tracker
    // will be removed from this map.
    std::map<NamespaceString, std::shared_ptr<CollectionSampleTracker>> _trackers;

    // Set of collections that have been sampled, for maintaining the total count of
    // collections sampled, reported in server status.
    std::set<NamespaceString> _sampledNamespaces;
};

}  // namespace analyze_shard_key
}  // namespace mongo
