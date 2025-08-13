/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace durable_catalog {

/**
 * Collection metadata persisted in an '_mdb_catalog' entry. Includes metadata details for each
 * 'index' in the collection.
 */
struct CatalogEntryMetaData {
    /*
     * Metadata for a given index.
     */
    struct IndexMetaData {
        IndexMetaData() {}

        IndexMetaData(const IndexMetaData& other)
            : spec(other.spec), ready(other.ready), buildUUID(other.buildUUID) {
            // We need to hold the multikey mutex when copying, someone else might be modifying this
            stdx::lock_guard lock(other.multikeyMutex);
            multikey = other.multikey;
            multikeyPaths = other.multikeyPaths;
        }

        IndexMetaData(IndexMetaData&& other)
            : spec(std::move(other.spec)),
              ready(other.ready),
              buildUUID(std::move(other.buildUUID)) {
            // No need to hold mutex on move, there are no concurrent readers while we're moving
            // the instance.
            multikey = other.multikey;
            multikeyPaths = std::move(other.multikeyPaths);
        }

        /**
         * An index is considered present if it has a non-empty 'spec'.
         * Invalid indexes by this definition include default constructed instances and
         * and structs zeroed out due to index drops.
         */
        bool isPresent() const {
            return !spec.isEmpty();
        }

        IndexMetaData& operator=(IndexMetaData&& rhs) {
            if (&rhs != this) {
                spec = std::move(rhs.spec);
                ready = std::move(rhs.ready);
                buildUUID = std::move(rhs.buildUUID);

                // No need to hold mutex on move, there are no concurrent readers while we're moving
                // the instance.
                multikey = std::move(rhs.multikey);
                multikeyPaths = std::move(rhs.multikeyPaths);
            }
            return *this;
        }

        void updateTTLSetting(long long newExpireSeconds);

        void updateHiddenSetting(bool hidden);

        void updateUniqueSetting(bool unique);

        void updatePrepareUniqueSetting(bool prepareUnique);

        StringData nameStringData() const {
            return spec["name"].valueStringDataSafe();
        }

        BSONObj spec;
        bool ready = false;

        // If initialized, a two-phase index build is in progress.
        boost::optional<UUID> buildUUID;

        // If non-empty, 'multikeyPaths' is a vector with size equal to the number of elements in
        // the index key pattern. Each element in the vector is an ordered set of positions
        // (starting at 0) into the corresponding indexed field that represent what prefixes of the
        // indexed field cause the index to be multikey.
        // multikeyMutex must be held when accessing multikey or multikeyPaths
        mutable stdx::mutex multikeyMutex;
        mutable bool multikey = false;
        mutable MultikeyPaths multikeyPaths;
        mutable AtomicWord<int32_t> concurrentWriters;
    };


    void parse(const BSONObj& obj);

    /**
     * If we have exclusive access to this MetaData (holding a unique copy). We don't need to
     * hold mutexes when reading internal data.
     */
    BSONObj toBSON(bool hasExclusiveAccess = false) const;

    /**
     * Returns number of valid indexes.
     */
    int getTotalIndexCount() const;

    int findIndexOffset(StringData name) const;

    /**
     * Inserts information about an index into the MetaData.
     */
    void insertIndex(IndexMetaData indexMetaData);

    /**
     * Removes information about an index from the MetaData. Returns true if an index
     * called name existed and was deleted, and false otherwise.
     */
    bool eraseIndex(StringData name);

    NamespaceString nss;
    CollectionOptions options;
    // May include empty instances which represent indexes already dropped.
    std::vector<IndexMetaData> indexes;

    // Note that collection cloning (initial sync, mongodump+mongorestore, resharding, etc.)
    // use listCollections and listIndexes to obtain the collection metadata. Therefore, any
    // additional field not contained inside options or indexes does not get cloned into the
    // target collection, which is almost surely problematic; see SERVER-91195 for more details.

    // Time-series collections created in versions 5.1 and earlier are allowed to contain
    // measurements with arbitrarily mixed schema in the buckets. When upgrading from these
    // earlier versions and setting FCV to 5.2 and up, this flag will be set to true by default
    // for existing time-series collections. To set the flag to false, all of the buckets need
    // to be checked for mixed-schema data. Newly created time-series collections in FCV 5.2 and
    // up will have this flag set to false by default. This will be boost::none if this catalog
    // entry is not representing a time-series collection or if FCV < 5.2.
    boost::optional<bool> timeseriesBucketsMayHaveMixedSchemaData;

    // Legacy time-series bucketing parameters changed flag. Due to SERVER-91193, this field is
    // unreliable and must not be used. Use the flag stored in the collection options instead.
    // TODO(SERVER-101423): Remove once 9.0 becomes last LTS.
    boost::optional<bool> timeseriesBucketingParametersHaveChanged_DO_NOT_USE;

    // Parsed value of the time-series mixed-schema flag stored in the backwards-compatible
    // field in the collection options (md.options.storageEngine.wiredTiger.configString).
    boost::optional<bool> _durableTimeseriesBucketsMayHaveMixedSchemaData;

    // Value of the time-series bucketing parameters changed flag in the backwards-compatible
    // field in the collection options (md.options.storageEngine.wiredTiger.configString).
    // The flag will be set to false at the time of time-series collection creation if
    // TSBucketingParametersUnchanged is enabled. For any other collection type and earlier
    // versions the flag will be boost::none. Thus, if the field is absent, we assume the
    // time-series bucketing parameters have changed. If a subsequent collMod operation changes
    // either 'bucketRoundingSeconds' or 'bucketMaxSpanSeconds', we set the flag to true.
    boost::optional<bool> _durableTimeseriesBucketingParametersHaveChanged;
};

}  // namespace durable_catalog
}  // namespace mongo
