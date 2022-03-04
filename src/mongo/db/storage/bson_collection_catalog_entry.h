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
#include <vector>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/tenant_namespace.h"

namespace mongo {

/**
 * This is a helper class for any storage engine that wants to store catalog information
 * as BSON. It is totally optional to use this.
 */
class BSONCollectionCatalogEntry {
public:
    /**
     * Incremented when breaking changes are made to the index build procedure so that other servers
     * know whether or not to resume or discard unfinished index builds.
     */
    static constexpr int kIndexBuildVersion = 1;

    BSONCollectionCatalogEntry() = default;

    virtual ~BSONCollectionCatalogEntry() {}

    // ------ for implementors

    struct IndexMetaData {
        IndexMetaData() {}

        IndexMetaData(const IndexMetaData& other)
            : spec(other.spec),
              ready(other.ready),
              isBackgroundSecondaryBuild(other.isBackgroundSecondaryBuild),
              buildUUID(other.buildUUID) {
            // We need to hold the multikey mutex when copying, someone else might be modifying this
            stdx::lock_guard lock(other.multikeyMutex);
            multikey = other.multikey;
            multikeyPaths = other.multikeyPaths;
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
                isBackgroundSecondaryBuild = std::move(rhs.isBackgroundSecondaryBuild);
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
        bool isBackgroundSecondaryBuild = false;

        // If initialized, a two-phase index build is in progress.
        boost::optional<UUID> buildUUID;

        // If non-empty, 'multikeyPaths' is a vector with size equal to the number of elements in
        // the index key pattern. Each element in the vector is an ordered set of positions
        // (starting at 0) into the corresponding indexed field that represent what prefixes of the
        // indexed field cause the index to be multikey.
        // multikeyMutex must be held when accessing multikey or multikeyPaths
        mutable Mutex multikeyMutex;
        mutable bool multikey = false;
        mutable MultikeyPaths multikeyPaths;
    };

    struct MetaData {
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

        TenantNamespace tenantNs;
        CollectionOptions options;
        std::vector<IndexMetaData> indexes;

        // Time-series collections created in versions 5.1 and earlier are allowed to contain
        // measurements with arbitrarily mixed schema in the buckets. When upgrading from these
        // earlier versions and setting FCV to 5.2 and up, this flag will be set to true by default
        // for existing time-series collections. To set the flag to false, all of the buckets need
        // to be checked for mixed-schema data. Newly created time-series collections in FCV 5.2 and
        // up will have this flag set to false by default. This will be boost::none if this catalog
        // entry is not representing a time-series collection or if FCV < 5.2.
        boost::optional<bool> timeseriesBucketsMayHaveMixedSchemaData;
    };
};
}  // namespace mongo
