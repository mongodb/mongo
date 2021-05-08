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

#include "mongo/db/index/index_descriptor_fwd.h"

#include <set>
#include <string>

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"

namespace mongo {

class IndexCatalogEntry;
class OperationContext;

/**
 * A cache of information computed from the memory-mapped per-index data (OnDiskIndexData).
 * Contains accessors for the various immutable index parameters, and an accessor for the
 * mutable "head" pointer which is index-specific.
 *
 * All synchronization is the responsibility of the caller.
 */
class IndexDescriptor {
public:
    enum class IndexVersion { kV1 = 1, kV2 = 2 };
    static constexpr IndexVersion kLatestIndexVersion = IndexVersion::kV2;

    // Used to report the result of a comparison between two indexes.
    enum class Comparison {
        kDifferent,   // Indicates that the indexes do not match.
        kEquivalent,  // Indicates that the options which uniquely identify an index match.
        kIdentical    // Indicates that all applicable index options match.
    };

    static constexpr StringData k2dIndexBitsFieldName = "bits"_sd;
    static constexpr StringData k2dIndexMinFieldName = "min"_sd;
    static constexpr StringData k2dIndexMaxFieldName = "max"_sd;
    static constexpr StringData k2dsphereCoarsestIndexedLevel = "coarsestIndexedLevel"_sd;
    static constexpr StringData k2dsphereFinestIndexedLevel = "finestIndexedLevel"_sd;
    static constexpr StringData k2dsphereVersionFieldName = "2dsphereIndexVersion"_sd;
    static constexpr StringData kBackgroundFieldName = "background"_sd;
    static constexpr StringData kCollationFieldName = "collation"_sd;
    static constexpr StringData kDefaultLanguageFieldName = "default_language"_sd;
    static constexpr StringData kDropDuplicatesFieldName = "dropDups"_sd;
    static constexpr StringData kExpireAfterSecondsFieldName = "expireAfterSeconds"_sd;
    static constexpr StringData kGeoHaystackBucketSize = "bucketSize"_sd;
    static constexpr StringData kHiddenFieldName = "hidden"_sd;
    static constexpr StringData kIndexNameFieldName = "name"_sd;
    static constexpr StringData kIndexVersionFieldName = "v"_sd;
    static constexpr StringData kKeyPatternFieldName = "key"_sd;
    static constexpr StringData kLanguageOverrideFieldName = "language_override"_sd;
    static constexpr StringData kNamespaceFieldName = "ns"_sd;  // Removed in 4.4
    static constexpr StringData kPartialFilterExprFieldName = "partialFilterExpression"_sd;
    static constexpr StringData kPathProjectionFieldName = "wildcardProjection"_sd;
    static constexpr StringData kSparseFieldName = "sparse"_sd;
    static constexpr StringData kStorageEngineFieldName = "storageEngine"_sd;
    static constexpr StringData kTextVersionFieldName = "textIndexVersion"_sd;
    static constexpr StringData kUniqueFieldName = "unique"_sd;
    static constexpr StringData kWeightsFieldName = "weights"_sd;

    /**
     * infoObj is a copy of the index-describing BSONObj contained in the catalog.
     */
    IndexDescriptor(const std::string& accessMethodName, BSONObj infoObj);

    /**
     * Returns true if the specified index version is supported, and returns false otherwise.
     */
    static bool isIndexVersionSupported(IndexVersion indexVersion);

    /**
     * Returns the index version to use if it isn't specified in the index specification.
     */
    static IndexVersion getDefaultIndexVersion();

    //
    // Information about the key pattern.
    //

    /**
     * Return the user-provided index key pattern.
     * Example: {geo: "2dsphere", nonGeo: 1}
     * Example: {foo: 1, bar: -1}
     */
    const BSONObj& keyPattern() const {
        return _keyPattern;
    }

    /**
     * Return the path projection spec, if one exists. This is only applicable for '$**' indexes.
     */
    const BSONObj& pathProjection() const {
        return _projection;
    }

    /**
     * Returns the normalized path projection spec, if one exists. This is only applicable for '$**'
     * indexes.
     */
    const BSONObj& normalizedPathProjection() const {
        return _normalizedProjection;
    }

    // How many fields do we index / are in the key pattern?
    int getNumFields() const {
        return _numFields;
    }

    //
    // Information about the index's namespace / collection.
    //

    // Return the name of the index.
    const std::string& indexName() const {
        return _indexName;
    }

    // Return the name of the access method we must use to access this index's data.
    const std::string& getAccessMethodName() const {
        return _accessMethodName;
    }

    // Returns the type of the index associated with this descriptor.
    IndexType getIndexType() const {
        return _indexType;
    }

    /**
     * Return a pointer to the IndexCatalogEntry that owns this descriptor, or null if orphaned.
     */
    IndexCatalogEntry* getEntry() const {
        return _entry;
    }

    //
    // Properties every index has
    //

    // Return what version of index this is.
    IndexVersion version() const {
        return _version;
    }

    // May each key only occur once?
    bool unique() const {
        return _unique;
    }

    bool hidden() const {
        return _hidden;
    }

    // Is this index sparse?
    bool isSparse() const {
        return _sparse;
    }

    // Is this a partial index?
    bool isPartial() const {
        return _partial;
    }

    bool isIdIndex() const {
        return _isIdIndex;
    }

    // Return a (rather compact) std::string representation.
    std::string toString() const {
        return _infoObj.toString();
    }

    // Return the info object.
    const BSONObj& infoObj() const {
        return _infoObj;
    }

    BSONObj toBSON() const {
        return _infoObj;
    }

    /**
     * Compares the current IndexDescriptor against the given existing index entry 'existingIndex'.
     * Returns kIdentical if all index options are logically identical, kEquivalent if all options
     * which uniquely identify an index are logically identical, and kDifferent otherwise.
     */
    Comparison compareIndexOptions(OperationContext* opCtx,
                                   const NamespaceString& ns,
                                   const IndexCatalogEntry* existingIndex) const;

    const BSONObj& collation() const {
        return _collation;
    }

    const BSONObj& partialFilterExpression() const {
        return _partialFilterExpression;
    }

    /**
     * Returns true if the key pattern is for the _id index.
     * The _id index must have form exactly {_id : 1} or {_id : -1}.
     * Allows an index of form {_id : "hashed"} to exist but
     * Do not consider it to be the primary _id index
     */
    static bool isIdIndexPattern(const BSONObj& pattern) {
        BSONObjIterator iter(pattern);
        BSONElement firstElement = iter.next();
        if (iter.next()) {
            return false;
        }
        if (firstElement.fieldNameStringData() != "_id"_sd) {
            return false;
        }
        auto intVal = firstElement.safeNumberInt();
        return intVal == 1 || intVal == -1;
    }

private:
    // This method should only ever be called by WildcardAccessMethod, to set the
    // '_normalizedProjection' for descriptors associated with an existing IndexCatalogEntry.
    void _setNormalizedPathProjection(BSONObj&& proj) {
        _normalizedProjection = std::move(proj);
    }

    // What access method should we use for this index?
    std::string _accessMethodName;

    IndexType _indexType;

    // The BSONObj describing the index.  Accessed through the various members above.
    BSONObj _infoObj;

    // --- cached data from _infoObj

    int64_t _numFields;  // How many fields are indexed?
    BSONObj _keyPattern;
    BSONObj _projection;
    BSONObj _normalizedProjection;
    std::string _indexName;
    bool _isIdIndex;
    bool _sparse;
    bool _unique;
    bool _hidden;
    bool _partial;
    IndexVersion _version;
    BSONObj _collation;
    BSONObj _partialFilterExpression;

    // Many query stages require going from an IndexDescriptor to its IndexCatalogEntry, so for
    // now we need this.
    IndexCatalogEntry* _entry = nullptr;

    friend class IndexCatalog;
    friend class IndexCatalogEntryImpl;
    friend class IndexCatalogEntryContainer;
    friend class WildcardAccessMethod;
};

}  // namespace mongo
