// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"
#include "mongo/util/modules_incompletely_marked_header.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <boost/intrusive_ptr.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/type_traits/decay.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
using namespace std::literals::string_view_literals;

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
    using IndexVersion = IndexConfig::IndexVersion;

    // Used to report the result of a comparison between two indexes.
    enum class Comparison {
        kDifferent,   // Indicates that the indexes do not match.
        kEquivalent,  // Indicates that the options which uniquely identify an index match.
        kIdentical    // Indicates that all applicable index options match.
    };

    static constexpr std::string_view k2dIndexBitsFieldName = "bits"sv;
    static constexpr std::string_view k2dIndexMinFieldName = "min"sv;
    static constexpr std::string_view k2dIndexMaxFieldName = "max"sv;
    static constexpr std::string_view k2dsphereCoarsestIndexedLevel = "coarsestIndexedLevel"sv;
    static constexpr std::string_view k2dsphereFinestIndexedLevel = "finestIndexedLevel"sv;
    static constexpr std::string_view k2dsphereVersionFieldName = "2dsphereIndexVersion"sv;
    static constexpr std::string_view kBackgroundFieldName = "background"sv;
    static constexpr std::string_view kBucketSizeFieldName = "bucketSize"sv;
    static constexpr std::string_view kCollationFieldName = "collation"sv;
    static constexpr std::string_view kDefaultLanguageFieldName = "default_language"sv;
    static constexpr std::string_view kDropDuplicatesFieldName = "dropDups"sv;
    static constexpr std::string_view kExpireAfterSecondsFieldName = "expireAfterSeconds"sv;
    static constexpr std::string_view kHiddenFieldName = "hidden"sv;
    static constexpr std::string_view kIndexNameFieldName = "name"sv;
    static constexpr std::string_view kIndexVersionFieldName = "v"sv;
    static constexpr std::string_view kKeyPatternFieldName = "key"sv;
    static constexpr std::string_view kLanguageOverrideFieldName = "language_override"sv;
    // TODO(SERVER-100328): remove after 9.0 is branched.
    static constexpr std::string_view kNamespaceFieldName = "ns"sv;  // Removed in 4.4
    static constexpr std::string_view kPartialFilterExprFieldName = "partialFilterExpression"sv;
    static constexpr std::string_view kWildcardProjectionFieldName = "wildcardProjection"sv;
    static constexpr std::string_view kSparseFieldName = "sparse"sv;
    static constexpr std::string_view kStorageEngineFieldName = "storageEngine"sv;
    static constexpr std::string_view kTextVersionFieldName = "textIndexVersion"sv;
    static constexpr std::string_view kUniqueFieldName = "unique"sv;
    static constexpr std::string_view kWeightsFieldName = "weights"sv;
    static constexpr std::string_view kOriginalSpecFieldName = "originalSpec"sv;
    static constexpr std::string_view kPrepareUniqueFieldName = "prepareUnique"sv;
    static constexpr std::string_view kClusteredFieldName = "clustered"sv;

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
        return _shared->_keyPattern;
    }

    /**
     * Return the path projection spec, if one exists. This is only applicable for wildcard ('$**')
     * indexes. It is kept as originally specified by the createIndex() call, not normalized.
     *
     * It contains only the projection object that was contained in one of the fields listed below
     * from the original createIndex() parameters object, but it does NOT preserve the field name:
     *   - "wildcardProjection"    (IndexDescriptor::kWildcardProjectionFieldName)
     *
     * This is set by the IndexDescriptor constructor and never changes after that.
     *
     * Example: db.a.createIndex({"$**":1}, {"name": "i1", "wildcardProjection": {"a.b": 1}})
     *   return (unnormalized) object: {"a.b":{"$numberDouble":"1"}}
     */
    const BSONObj& pathProjection() const {
        return _shared->_projection;
    }

    /**
     * Returns the normalized path projection spec, if one exists. This is only applicable for
     * wildcard ('$**') indexes. It is the normalized version of the path projection
     * and is used to determine whether a new index candidate from createIndex() duplicates an
     * existing index.
     *
     * It contains the normalized projection object based on the original object that was contained
     * in one of the fields listed below from the original createIndex() parameters object, but it
     * does NOT preserve the field name:
     *   - "wildcardProjection"    (IndexDescriptor::kWildcardProjectionFieldName)
     *
     * This is set by the IndexDescriptor constructor and never changes after that.
     *
     * Example: db.a.createIndex({"$**":1}, {"name": "i1", "wildcardProjection": {"a.b": 1}})
     *   return (normalized) object: {"a":{"b":true},"_id":false}
     */
    const BSONObj& normalizedPathProjection() const {
        return _shared->_normalizedProjection;
    }

    // How many fields do we index / are in the key pattern?
    int getNumFields() const {
        return _shared->_numFields;
    }

    //
    // Information about the index's namespace / collection.
    //

    // Return the name of the index.
    const std::string& indexName() const {
        return _shared->_indexName;
    }

    // Return the name of the access method we must use to access this index's data.
    const std::string& getAccessMethodName() const {
        return _shared->_accessMethodName;
    }

    // Returns the type of the index associated with this descriptor.
    IndexType getIndexType() const {
        return _shared->_indexType;
    }

    //
    // Properties every index has
    //

    // Return what version of index this is.
    IndexVersion version() const {
        return _shared->_version;
    }

    // Return the 'Ordering' of the index keys.
    const Ordering& ordering() const {
        return _shared->_ordering;
    }

    // May each key only occur once?
    bool unique() const {
        return _shared->_unique;
    }

    bool hidden() const {
        return _shared->_hidden;
    }

    // Is this index set as sparse by the user?
    bool isSetSparseByUser() const {
        return _shared->_sparse;
    }

    // Is this index set as partial by the user?
    bool isPartial() const {
        return _shared->_partial;
    }

    // Whether this index behaves as sparse, independent of the user settings. Sphere2d and
    // wildcard indexes are inherently sparse.
    bool behavesAsSparse() const {
        return _shared->_sparse || _shared->_indexType == IndexType::INDEX_WILDCARD ||
            (_shared->_indexType == IndexType::INDEX_2DSPHERE &&
             _shared->_version >= IndexVersion::kV2);
    }

    bool isIdIndex() const {
        return _shared->_isIdIndex;
    }

    bool isHashedIdIndex() const {
        return _shared->_isHashedIdIndex;
    }

    // Return a (rather compact) std::string representation.
    std::string toString() const {
        return _shared->_infoObj.toString();
    }

    // Converts to a minimal type for passing into storage engine.
    IndexConfig toIndexConfig() const {
        return {isIdIndex(), unique(), version(), infoObj(), indexName(), ordering()};
    }

    // Return the info object.
    const BSONObj& infoObj() const {
        return _shared->_infoObj;
    }

    BSONObj toBSON() const {
        return _shared->_infoObj;
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
        return _shared->_collation;
    }

    const BSONObj& partialFilterExpression() const {
        return _shared->_partialFilterExpression;
    }

    bool prepareUnique() const {
        return _shared->_prepareUnique;
    }

    boost::optional<std::string_view> compressor() const {
        return _shared->_compressor ? boost::make_optional<std::string_view>(*_shared->_compressor)
                                    : boost::none;
    }

    /**
     * Returns the field names from the index key pattern.
     *
     * Examples:
     * For the index key pattern {a: 1, b: 1}, this method returns {"a", "b"}.
     * For the text index key pattern {a: "text", _fts: "text", b: "text"}, this method returns
     * {"a", "term", "weight", "b"}.
     *
     * Note that this method will not be able to resolve the field names for a wildcard index. So,
     * for the wild card index {"$**": 1}, this method will return {"$**"}.
     */
    std::vector<const char*> getFieldNames() const;

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
        if (firstElement.fieldNameStringData() != "_id"sv) {
            return false;
        }
        auto intVal = firstElement.safeNumberInt();
        return intVal == 1 || intVal == -1;
    }

    /**
     * Returns true if the key pattern is for the _id index and is hashed.
     * The _id index must have form exactly {_id : "hashed"}.
     */
    static bool isHashedIdIndex(const BSONObj& pattern) {
        BSONObjIterator iter(pattern);
        BSONElement firstElement = iter.next();
        if (iter.next()) {
            return false;
        }
        if (firstElement.fieldNameStringData() != "_id"sv) {
            return false;
        }
        auto strVal = firstElement.valueStringDataSafe();
        return strVal == "hashed"sv;
    }

private:
    /*
     * Holder of shared state between IndexDescriptor clones.
     */
    struct SharedState : public RefCountable {
        SharedState(const std::string& accessMethodName, BSONObj infoObj);

        // What access method should we use for this index?
        std::string _accessMethodName;

        IndexType _indexType;

        // The BSONObj describing the index.  Accessed through the various members above.
        BSONObj _infoObj;

        // --- cached data from _infoObj

        int64_t _numFields;  // How many fields are indexed?
        BSONObj _keyPattern;
        BSONObj _projection;            // for wildcardProjection; never changes
        BSONObj _normalizedProjection;  // for wildcardProjection; never changes
        std::string _indexName;
        bool _isIdIndex;
        bool _isHashedIdIndex;
        bool _sparse;
        bool _unique;
        bool _hidden;
        bool _partial;
        IndexVersion _version;
        // '_ordering' should be initialized after '_indexType' because different index types may
        // require different handling of the Ordering.
        Ordering _ordering;
        BSONObj _collation;
        BSONObj _partialFilterExpression;
        bool _prepareUnique = false;
        boost::optional<std::string> _compressor;
    };

    boost::intrusive_ptr<SharedState> _shared;
};

}  // namespace mongo
