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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/local_catalog/collection_type.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <absl/hash/hash.h>
#include <boost/optional/optional.hpp>

namespace mongo::query_stats {

/**
 * A struct holding pieces of the command request that are a component of the query stats store key
 * and are options/arguments to all supported query stats commands.
 *
 * This struct (and the SpecificKeyComponents) are split out as a separate inheritence hierarchy to
 * make it easier to ensure each piece is hashed without sub-classes needing to enumerate the parent
 * class's member variables.
 */
struct UniversalKeyComponents {
    UniversalKeyComponents(std::unique_ptr<query_shape::Shape> queryShape,
                           const ClientMetadata* clientMetadata,
                           boost::optional<BSONObj> commentObj,
                           boost::optional<BSONObj> hint,
                           boost::optional<BSONObj> readPreference,
                           boost::optional<BSONObj> writeConcern,
                           boost::optional<BSONObj> readConcern,
                           std::unique_ptr<APIParameters> apiParams,
                           query_shape::CollectionType collectionType,
                           bool maxTimeMS);
    /**
     * Returns a copy of the read concern object. If there is an "afterClusterTime" or
     * "atClusterTime" component, the timestamp is shapified according to 'opts'.
     */
    static BSONObj shapifyReadConcern(
        const BSONObj& readConcern,
        const SerializationOptions& opts =
            SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    size_t size() const;

    void appendTo(BSONObjBuilder& bob, const SerializationOptions& opts) const;

    // Avoid using boost::optional here because it creates extra padding at the beginning of the
    // struct. Since each QueryStatsEntry has its own Key subclass, it's better to minimize
    // the struct's size as much as possible.

    BSONObj _clientMetaData;  // Preserve this value.
    BSONObj _commentObj;      // Shapify this value.
    BSONObj _hintObj;         // Preserve this value.
    BSONObj _writeConcern;    // Preserve this value.

    // Preserved literal except value of 'tags' field is sorted.
    BSONObj _shapifiedReadPreference;
    // Preserved literal except 'afterClusterTime' and 'atClusterTime' are shapified.
    BSONObj _shapifiedReadConcern;

    // Separate the possibly-enormous BSONObj from the remaining members

    BSONElement _comment;

    std::unique_ptr<query_shape::Shape> _queryShape;
    std::unique_ptr<APIParameters> _apiParams;  // Preserve this value in the query shape.

    // Simple hash of the client metadata object. This value is stored separately because it is
    // cached on the client to avoid re-computing on every operation. If no client metadata is
    // present, this will be the hash of an empty BSON object (otherwise known as 0).
    const unsigned long _clientMetaDataHash;

    // This value is not known when run a query is run on mongos over an unsharded collection, so it
    // is not set through that code path.
    query_shape::CollectionType _collectionType;

    // The tenant id associated with the collection targeted by the query if '_hasField.tenantId' is
    // set.
    const TenantId _tenantId;

    // This anonymous struct represents the presence of the member variables as C++ bit fields.
    // In doing so, each of these boolean values takes up 1 bit instead of 1 byte.
    struct HasField {
        bool clientMetaData : 1 = false;
        bool comment : 1 = false;
        bool hint : 1 = false;
        bool readPreference : 1 = false;
        bool writeConcern : 1 = false;
        bool readConcern : 1 = false;
        bool maxTimeMS : 1 = false;
        bool tenantId : 1 = false;
    } _hasField;
};

/**
 * A base class for sub-classes to derive from to expose the hashing ability for all of their
 * sub-components.
 *
 * This struct (and the UniversalKeyComponents) are split out as a separate inheritence hierarchy to
 * make it easier to ensure each piece is hashed without sub-classes needing to enumerate the parent
 * class's member variables.
 */
struct SpecificKeyComponents {
    virtual ~SpecificKeyComponents() {}

    virtual void HashValue(absl::HashState state) const = 0;

    /**
     * Sub-classes should implement this to report how much memory is used. This is important to do
     * carefully since we are under a budget in the query stats store and use this to do the
     * accounting. Implementers should include sizeof(*derivedThis) and be sure to also include the
     * size of any owned pointer-like objects such as BSONObj or NamespaceString which are
     * indirectly using memory elsehwhere.
     *
     * We cannot just use sizeof() because there are some variable size data members (like BSON
     * objects) which depend on the particular instance.
     */
    virtual size_t size() const = 0;
};

template <typename H>
H AbslHashValue(H state, const SpecificKeyComponents& value) {
    value.HashValue(absl::HashState::Create(&state));
    return std::move(state);
}

template <typename H>
H AbslHashValue(H h, const UniversalKeyComponents& components) {
    return H::combine(std::move(h),
                      *components._queryShape,
                      components._clientMetaDataHash,
                      // Note we use the comment's type in the hash function.
                      components._comment.type(),
                      simpleHash(components._hintObj),
                      simpleHash(components._shapifiedReadPreference),
                      simpleHash(components._writeConcern),
                      simpleHash(components._shapifiedReadConcern),
                      components._apiParams ? APIParameters::Hash{}(*components._apiParams) : 0,
                      components._collectionType,
                      components._hasField);
}

template <typename H>
H AbslHashValue(H h, const UniversalKeyComponents::HasField& hasField) {
    return H::combine(std::move(h),
                      hasField.clientMetaData,
                      hasField.comment,
                      hasField.hint,
                      hasField.readPreference,
                      hasField.writeConcern,
                      hasField.readConcern,
                      hasField.maxTimeMS,
                      hasField.tenantId);
}


// This static assert checks to ensure that the struct's size is changed thoughtfully. If adding
// or otherwise changing the members, this assert may be updated with care.
static_assert(
    sizeof(UniversalKeyComponents) <= sizeof(query_shape::Shape) + 6 * sizeof(BSONObj) +
            sizeof(BSONElement) + sizeof(std::unique_ptr<APIParameters>) +
            sizeof(query_shape::CollectionType) + sizeof(query_shape::QueryShapeHash) +
            sizeof(int64_t),
    "Size of Key is too large! "
    "Make sure that the struct has been align- and padding-optimized. "
    "If the struct's members have changed, this assert may need to be updated with a new value.");


/**
 * A sub-class used for commmands without any specific key components, such as count and distinct.
 */
struct EmptyCmdComponents : public SpecificKeyComponents {
    EmptyCmdComponents() {};

    std::size_t size() const override {
        return sizeof(EmptyCmdComponents);
    }

    void HashValue(absl::HashState state) const final {}
};


/**
 * An abstract base class representing a query stats store key for a given request. All query stats
 * store entries should include some common elements, tracked in `_universalComponents`.  For
 * example, everything tracked must have a `query_shape::Shape`.
 *
 * Subclasses can add more components to include as discriminating factors in which entries should
 * be tracked separately. For example, two find commands which are identical except in their read
 * concern should be tracked differently. Maybe they will have quite different performance
 * characteristics or help us determine when the read concern was changed by the client.
 *
 * The interface to do this is to split out the state/memory for these components as a separate
 * struct which can indpendently hash itself and compute its size (both of which are important for
 * the query stats store). Subclasses of Key itself should not have any meaningfully sized
 * state other than the 'specificComponents().'
 */
class Key {
public:
    virtual ~Key() = default;

    /**
     * All Keys will share these characteristics as part of their query stats store key.
     * Returns an unowned reference so the caller must ensure the result does not outlive this
     * Key instance.
     */
    const auto& universalComponents() const {
        return _universalComponents;
    }

    /**
     * Different commands will have different components they want to be included in the query stats
     * store key. This interface allows them to do so and easily have those components incorporated
     * into this key generation and hashing.
     */
    virtual const SpecificKeyComponents& specificComponents() const = 0;

    /**
     * Materializes the query stats store key. Not expected to be used on ingestion, since we should
     * store this object and its components directly in their native C++ data structures - we can
     * use the absl::HashOf() API to look them up. Instead, this may be useful to display the key
     * (as it is used for $queryStats) or perhaps one day persist it to storage.
     */
    BSONObj toBson(OperationContext* opCtx,
                   const SerializationOptions& opts,
                   const SerializationContext& serializationContext) const;

    /**
     * Convenience function.
     */
    query_shape::QueryShapeHash getQueryShapeHash(
        OperationContext* opCtx, const SerializationContext& serializationContext) const {
        // TODO (future ticket?) should we cache this somewhere else?
        return _universalComponents._queryShape->sha256Hash(opCtx, serializationContext);
    }

    size_t size() const {
        return sizeof(Key) + specificComponents().size() + _universalComponents.size();
    }

    template <typename H>
    friend H AbslHashValue(H h, const Key& key) {
        return H::combine(std::move(h), key._universalComponents, key.specificComponents());
    }

    // The default implementation of hashing for smart pointers is not a good one for our purposes.
    // Here we overload them to actually take the hash of the object, rather than hashing the
    // pointer itself.
    template <typename H>
    friend H AbslHashValue(H h, const std::unique_ptr<const Key>& key) {
        return H::combine(std::move(h), *key);
    }
    template <typename H>
    friend H AbslHashValue(H h, const std::shared_ptr<const Key>& key) {
        return H::combine(std::move(h), *key);
    }

protected:
    /**
     * Sub-classes can use this to instantiate a 'real' Key. 'queryShape' must not be null,
     * but is tracked as a pointer since it is a virtual class and we want to own it here.
     */
    Key(OperationContext* opCtx,
        std::unique_ptr<query_shape::Shape> queryShape,
        const boost::optional<BSONObj>& hint,
        const boost::optional<repl::ReadConcernArgs>& readConcern,
        bool hasMaxTimeMS,
        query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown);

    /**
     * With a given BSONObjBuilder, append the command-specific components of the query stats key.
     *
     * You may be wondering why this API is here rather than as a virtual method on
     * CmdSpecificComponents - and that would be because many implementations can involve a re-parse
     * of the request if it needs to serialize with different serialization options. This re-parsing
     * process often needs the context of things tracked in _universalComponents, which is hard to
     * access from the specific components.
     */
    virtual void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                                 const SerializationOptions& opts) const = 0;

private:
    UniversalKeyComponents _universalComponents;
};
static_assert(
    sizeof(Key) == sizeof(void*) /*vtable ptr*/ + sizeof(UniversalKeyComponents),
    "If the class' members have changed, this assert may need to be updated with a new value.");
}  // namespace mongo::query_stats
