/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/ops/single_write_result_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/views/view.h"
#include "mongo/util/string_map.h"

#include <queue>

namespace mongo {
class BucketCatalog {
public:
    // This constant, together with parameters defined in timeseries.idl, defines limits on the
    // measurements held in a bucket.
    static constexpr auto kTimeseriesBucketMaxTimeRange = Hours(1);

    class BucketId {
    public:
        const OID& operator*() const;
        const OID* operator->() const;

        bool operator==(const BucketId& other) const;
        bool operator<(const BucketId& other) const;

        template <typename H>
        friend H AbslHashValue(H h, const BucketId& bucketId) {
            return H::combine(std::move(h), bucketId._num);
        }

    protected:
        BucketId(uint64_t num) : _num(num) {}

        std::shared_ptr<OID> _id{std::make_shared<OID>(OID::gen())};

    private:
        uint64_t _num;
    };

    struct CommitInfo {
        StatusWith<SingleWriteResult> result;
        boost::optional<repl::OpTime> opTime;
        boost::optional<OID> electionId;
    };

    struct InsertResult {
        BucketId bucketId;
        boost::optional<Future<CommitInfo>> commitInfo;
    };

    struct CommitData {
        std::vector<BSONObj> docs;
        BSONObj bucketMin;  // The full min/max if this is the bucket's first commit, or the updates
        BSONObj bucketMax;  // since the previous commit if not.
        uint32_t numCommittedMeasurements;
        StringSet newFieldNamesToBeInserted;

        BSONObj toBSON() const;
    };

    static BucketCatalog& get(ServiceContext* svcCtx);
    static BucketCatalog& get(OperationContext* opCtx);

    BucketCatalog() = default;

    BucketCatalog(const BucketCatalog&) = delete;
    BucketCatalog operator=(const BucketCatalog&) = delete;

    /**
     * Returns the metadata for the given bucket in the following format:
     *     {<metadata field name>: <value>}
     * All measurements in the given bucket share same metadata value.
     *
     * Returns an empty document if the given bucket cannot be found or if this time-series
     * collection was not created with a metadata field name.
     */
    BSONObj getMetadata(const BucketId& bucketId) const;

    /**
     * Returns the id of the bucket that the document belongs in, and a Future to wait on if the
     * caller is a waiter for the bucket. If no Future is provided, the caller is the committer for
     * this bucket.
     */
    StatusWith<InsertResult> insert(OperationContext* opCtx,
                                    const NamespaceString& ns,
                                    const BSONObj& doc);

    /**
     * Returns the uncommitted measurements and the number of measurements that have already been
     * committed for the given bucket. This should be called continuously by the committer until
     * there are no more uncommitted measurements.
     */
    CommitData commit(const BucketId& bucketId,
                      boost::optional<CommitInfo> previousCommitInfo = boost::none);

    /**
     * Clears the given bucket.
     */
    void clear(const BucketId& bucketId);

    /**
     * Clears the buckets for the given namespace.
     */
    void clear(const NamespaceString& ns);

    /**
     * Clears the buckets for the given database.
     */
    void clear(StringData dbName);

    /**
     * Appends the execution stats for the given namespace to the builder.
     */
    void appendExecutionStats(const NamespaceString& ns, BSONObjBuilder* builder) const;

private:
    struct BucketMetadata {
        bool operator<(const BucketMetadata& other) const;
        bool operator==(const BucketMetadata& other) const;

        template <typename H>
        friend H AbslHashValue(H h, const BucketMetadata& metadata) {
            return H::combine(std::move(h),
                              UnorderedFieldsBSONObjComparator(metadata.view->defaultCollator())
                                  .hash(metadata.metadata));
        }

        BSONObj metadata;
        std::shared_ptr<const ViewDefinition> view;
    };

    class MinMax {
    public:
        /*
         * Updates the min/max according to 'comp', ignoring the 'metaField' field.
         */
        void update(const BSONObj& doc,
                    boost::optional<StringData> metaField,
                    const StringData::ComparatorInterface* stringComparator,
                    const std::function<bool(int, int)>& comp);

        /**
         * Returns the full min/max object.
         */
        BSONObj toBSON() const;

        /**
         * Returns the updates since the previous time this function was called in the format for
         * an update op.
         */
        BSONObj getUpdates();

        /*
         * Returns the approximate memory usage of this MinMax.
         */
        uint64_t getMemoryUsage() const;

    private:
        enum class Type {
            kObject,
            kArray,
            kValue,
            kUnset,
        };

        void _update(BSONElement elem,
                     const StringData::ComparatorInterface* stringComparator,
                     const std::function<bool(int, int)>& comp);
        void _updateWithMemoryUsage(MinMax* minMax,
                                    BSONElement elem,
                                    const StringData::ComparatorInterface* stringComparator,
                                    const std::function<bool(int, int)>& comp);

        void _append(BSONObjBuilder* builder) const;
        void _append(BSONArrayBuilder* builder) const;

        /*
         * Appends updates, if any, to the builder. Returns whether any updates were appended by
         * this MinMax or any MinMaxes below it.
         */
        bool _appendUpdates(BSONObjBuilder* builder);

        /*
         * Clears the '_updated' flag on this MinMax and all MinMaxes below it.
         */
        void _clearUpdated();

        StringMap<MinMax> _object;
        std::vector<MinMax> _array;
        BSONObj _value;

        Type _type = Type::kUnset;

        bool _updated = false;

        uint64_t _memoryUsage = 0;
    };

    struct Bucket {
        // The namespace that this bucket is used for.
        NamespaceString ns;

        // The metadata of the data that this bucket contains.
        BucketMetadata metadata;

        // Measurements to be inserted into the bucket.
        std::vector<BSONObj> measurementsToBeInserted;

        // New top-level field names of the measurements to be inserted.
        StringSet newFieldNamesToBeInserted;

        // Top-level field names of the measurements that have been inserted into the bucket.
        StringSet fieldNames;

        // The minimum values for each field in the bucket.
        MinMax min;

        // The maximum values for each field in the bucket.
        MinMax max;

        // The latest time that has been inserted into the bucket.
        Date_t latestTime;

        // The total size in bytes of the bucket's BSON serialization, including measurements to be
        // inserted.
        uint64_t size = 0;

        // The total number of measurements in the bucket, including uncommitted measurements and
        // measurements to be inserted.
        uint32_t numMeasurements = 0;

        // The number of measurements that were most recently returned from a call to commit().
        uint32_t numPendingCommitMeasurements = 0;

        // The number of committed measurements in the bucket.
        uint32_t numCommittedMeasurements = 0;

        // The number of current writers for the bucket.
        uint32_t numWriters = 0;

        // Promises for committers to fulfill in order to signal to waiters that their measurements
        // have been committed.
        std::queue<Promise<CommitInfo>> promises;

        // Whether the bucket is full. This can be due to number of measurements, size, or time
        // range.
        bool full = false;

        // Approximate memory usage of this bucket.
        uint64_t memoryUsage = sizeof(*this);

        /**
         * Determines the effect of adding 'doc' to this bucket If adding 'doc' causes this bucket
         * to overflow, we will create a new bucket and recalculate the change to the bucket size
         * and data fields.
         */
        void calculateBucketFieldsAndSizeChange(const BSONObj& doc,
                                                boost::optional<StringData> metaField,
                                                StringSet* newFieldNamesToBeInserted,
                                                uint32_t* newFieldNamesSize,
                                                uint32_t* sizeToBeAdded) const;

        /**
         * Returns whether BucketCatalog::commit has been called on this bucket.
         */
        bool hasBeenCommitted() const;
    };

    struct ExecutionStats {
        long long numBucketInserts = 0;
        long long numBucketUpdates = 0;
        long long numBucketsOpenedDueToMetadata = 0;
        long long numBucketsClosedDueToCount = 0;
        long long numBucketsClosedDueToSize = 0;
        long long numBucketsClosedDueToTimeForward = 0;
        long long numBucketsClosedDueToTimeBackward = 0;
        long long numBucketsClosedDueToMemoryThreshold = 0;
        long long numCommits = 0;
        long long numWaits = 0;
        long long numMeasurementsCommitted = 0;
    };

    class BucketIdInternal : public BucketId {
    public:
        static BucketIdInternal min();

        BucketIdInternal(const Date_t& time, uint64_t num);

        Date_t getTime() const;
        void setTime(const Date_t& time);
    };

    class ServerStatus;

    using NsBuckets = std::set<std::tuple<NamespaceString, BucketId>>;
    using IdleBuckets = std::set<BucketId>;

    /**
     * Removes the given bucket from the bucket catalog's internal data structures.
     */
    void _removeBucket(const BucketId& bucketId,
                       boost::optional<NsBuckets::iterator> nsBucketsIt = boost::none,
                       boost::optional<IdleBuckets::iterator> idleBucketsIt = boost::none);

    /**
     * Expires idle buckets until the bucket catalog's memory usage is below the expiry threshold.
     */
    void _expireIdleBuckets(ExecutionStats* stats);

    mutable Mutex _mutex = MONGO_MAKE_LATCH("BucketCatalog");

    // All buckets currently in the catalog, including buckets which are full but not yet committed.
    stdx::unordered_map<BucketId, Bucket> _buckets;

    // The _id of the current bucket for each namespace and metadata pair.
    stdx::unordered_map<std::tuple<NamespaceString, BucketMetadata>, BucketIdInternal> _bucketIds;

    // All buckets ordered by their namespaces.
    NsBuckets _nsBuckets;

    // Buckets that do not have any writers.
    IdleBuckets _idleBuckets;

    // Per-collection execution stats.
    stdx::unordered_map<NamespaceString, ExecutionStats> _executionStats;

    // Counter for buckets created by the bucket catalog.
    uint64_t _bucketNum = 0;

    // Approximate memory usage of the bucket catalog.
    uint64_t _memoryUsage = 0;
};
}  // namespace mongo
