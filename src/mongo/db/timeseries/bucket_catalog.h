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
#include "mongo/util/string_map.h"

namespace mongo {
class BucketCatalog {
public:
    // This set of constants define limits on the measurements held in a bucket.
    static constexpr int kTimeseriesBucketMaxCount = 1000;
    static constexpr int kTimeseriesBucketMaxSizeBytes = 125 * 1024;  // 125 KB
    static constexpr auto kTimeseriesBucketMaxTimeRange = Hours(1);

    struct CommitInfo {
        StatusWith<SingleWriteResult> result;
        boost::optional<repl::OpTime> opTime;
        boost::optional<OID> electionId;
    };

    struct InsertResult {
        OID bucketId;
        boost::optional<Future<CommitInfo>> commitInfo;
    };

    struct CommitData {
        std::vector<BSONObj> docs;
        BSONObj bucketMin;
        BSONObj bucketMax;
        uint16_t numCommittedMeasurements;
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
    BSONObj getMetadata(const OID& bucketId) const;

    /**
     * Returns the id of the bucket that the document belongs in, and a Future to wait on if the
     * caller is a waiter for the bucket. If no Future is provided, the caller is the committer for
     * this bucket.
     */
    InsertResult insert(OperationContext* opCtx, const NamespaceString& ns, const BSONObj& doc);

    /**
     * Returns the uncommitted measurements and the number of measurements that have already been
     * committed for the given bucket. This should be called continuously by the committer until
     * there are no more uncommitted measurements.
     */
    CommitData commit(const OID& bucketId,
                      boost::optional<CommitInfo> previousCommitInfo = boost::none);

    /**
     * Clears the buckets for the given namespace.
     */
    void clear(const NamespaceString& ns);

    /**
     * Clears the buckets for the given database.
     */
    void clear(StringData dbName);

private:
    struct BucketMetadata {
        bool operator<(const BucketMetadata& other) const;
        bool operator==(const BucketMetadata& other) const;

        template <typename H>
        friend H AbslHashValue(H h, const BucketMetadata& metadata) {
            return H::combine(std::move(h),
                              UnorderedFieldsBSONObjComparator().hash(metadata.metadata));
        }

        BSONObj metadata;
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
        BSONObj min;

        // The maximum values for each field in the bucket.
        BSONObj max;

        // The total size in bytes of the bucket's BSON serialization, including measurements to be
        // inserted.
        uint32_t size = 0;

        // The total number of measurements in the bucket, including uncommitted measurements and
        // measurements to be inserted.
        uint16_t numMeasurements = 0;

        // The number of measurements that were most recently returned from a call to commit().
        uint16_t numPendingCommitMeasurements = 0;

        // The number of committed measurements in the bucket.
        uint16_t numCommittedMeasurements = 0;

        // The number of current writers for the bucket.
        uint32_t numWriters = 0;

        // Promises for committers to fulfill in order to signal to waiters that their measurements
        // have been committed.
        stdx::unordered_map<uint16_t, Promise<CommitInfo>> promises;

        // Whether the bucket is full. This can be due to number of measurements, size, or time
        // range.
        bool full = false;
    };

    mutable Mutex _mutex = MONGO_MAKE_LATCH("BucketCatalog");

    // All buckets currently in the catalog, including buckets which are full but not yet committed.
    stdx::unordered_map<OID, Bucket, OID::Hasher> _buckets;

    // The _id of the current bucket for each namespace and metadata pair.
    stdx::unordered_map<std::pair<NamespaceString, BucketMetadata>, OID> _bucketIds;

    // All namespace, metadata, and _id tuples which currently have a bucket in the catalog.
    std::set<std::tuple<NamespaceString, BucketMetadata, OID>> _orderedBuckets;

    // Buckets that do not have any writers.
    std::set<OID> _idleBuckets;
};
}  // namespace mongo
