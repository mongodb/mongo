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
#include "mongo/db/storage/kv/kv_prefix.h"

namespace mongo {

/**
 * This is a helper class for any storage engine that wants to store catalog information
 * as BSON. It is totally optional to use this.
 */
class BSONCollectionCatalogEntry {
public:
    static const StringData kIndexBuildScanning;
    static const StringData kIndexBuildDraining;

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

        void updateTTLSetting(long long newExpireSeconds);

        std::string name() const {
            return spec["name"].String();
        }

        BSONObj spec;
        bool ready = false;
        bool multikey = false;
        KVPrefix prefix = KVPrefix::kNotPrefixed;
        bool isBackgroundSecondaryBuild = false;

        long versionOfBuild = kIndexBuildVersion;
        // If true, a two-phase index build is in progress, false otherwise.
        bool runTwoPhaseBuild = false;
        boost::optional<std::string> buildPhase;
        boost::optional<std::string> constraintViolationsIdent;
        boost::optional<std::string> sideWritesIdent;

        // If non-empty, 'multikeyPaths' is a vector with size equal to the number of elements in
        // the index key pattern. Each element in the vector is an ordered set of positions
        // (starting at 0) into the corresponding indexed field that represent what prefixes of the
        // indexed field cause the index to be multikey.
        MultikeyPaths multikeyPaths;
    };

    struct MetaData {
        void parse(const BSONObj& obj);
        BSONObj toBSON() const;

        int findIndexOffset(StringData name) const;

        /**
         * Removes information about an index from the MetaData. Returns true if an index
         * called name existed and was deleted, and false otherwise.
         */
        bool eraseIndex(StringData name);

        void rename(StringData toNS);

        KVPrefix getMaxPrefix() const;

        std::string ns;
        CollectionOptions options;
        std::vector<IndexMetaData> indexes;
        KVPrefix prefix = KVPrefix::kNotPrefixed;
    };
};
}  // namespace mongo
