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

#include <absl/hash/hash.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/collection_type.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/db/query/shape_helpers.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/decorable.h"

namespace mongo {

int64_t inline optionalObjSize(boost::optional<BSONObj> optionalObj) {
    if (!optionalObj)
        return 0;
    return optionalObj->objsize();
}

template <typename T>
int64_t optionalSize(boost::optional<T> optionalVal) {
    if (!optionalVal)
        return 0;
    return optionalVal->size();
}

namespace query_stats {

/**
 * An abstract base class to handle generating the query stats key for a given request.
 */
class KeyGenerator {
public:
    virtual ~KeyGenerator() = default;

    /**
     * Generate the query stats key with the given tokenization strategy.
     */
    virtual BSONObj generate(
        OperationContext* opCtx,
        boost::optional<SerializationOptions::TokenizeIdentifierFunc>) const = 0;

    /**
     * Compute the query stats key hash by combining the hash components for this specific command
     * with the pre-computed query shape hash.
     */
    size_t hash() const {
        BSONObjBuilder bob;
        // Rather than the query shape itself, insert its hash into the key.
        _queryShapeHash.appendAsBinData(bob, "queryShape");
        appendImmediateComponents(bob,
                                  SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
        bob.doneFast();
        return absl::hash_internal::CityHash64(bob.bb().buf(), bob.bb().len());
    }

    int64_t size() const {
        return doGetSize() +
            _parseableQueryShape.objsize() + /* _collectionType is not owned here */
            (_apiParams ? sizeof(*_apiParams) + optionalSize(_apiParams->getAPIVersion()) : 0) +
            (_hasField.clientMetaData ? _clientMetaData.objsize() : 0) + _commentObj.objsize() +
            (_hasField.readPreference ? _readPreference.objsize() : 0) + _hintObj.objsize();
    }

    BSONObj getRepresentativeQueryShapeForDebug() const {
        return _parseableQueryShape;
    }

    query_shape::QueryShapeHash getQueryShapeHash() const {
        return _queryShapeHash;
    }

protected:
    KeyGenerator(OperationContext* opCtx,
                 BSONObj parseableQueryShape,
                 boost::optional<BSONObj> hint,
                 query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown,
                 boost::optional<query_shape::QueryShapeHash> queryShapeHash = boost::none)
        : _parseableQueryShape(parseableQueryShape.getOwned()),
          _hintObj(hint.value_or(BSONObj())),
          _queryShapeHash(queryShapeHash.value_or(query_shape::hash(parseableQueryShape))),
          _collectionType(collectionType) {
        if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
            _clientMetaData = metadata->getDocument();
            _hasField.clientMetaData = true;
        }

        if (auto comment = opCtx->getCommentOwnedCopy()) {
            _commentObj = std::move(comment.value());
            _comment = _commentObj.firstElement();
            _hasField.comment = true;
        }

        _apiParams = std::make_unique<APIParameters>(APIParameters::get(opCtx));

        if (!ReadPreferenceSetting::get(opCtx).toInnerBSON().isEmpty() &&
            !ReadPreferenceSetting::get(opCtx).usedDefaultReadPrefValue()) {
            _readPreference = ReadPreferenceSetting::get(opCtx).toInnerBSON();
            _hasField.readPreference = true;
        }
    }

    /**
     * With a given BSONObjBuilder, append the command-specific components of the query stats key.
     */
    virtual void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                                 const SerializationOptions& opts) const = 0;

    /**
     * Helper function to generate the Query Stats Key, using the passed-in query shape as the
     * `queryShape` sub-object.
     */
    BSONObj generateWithQueryShape(BSONObj queryShape, const SerializationOptions& opts) const {
        BSONObjBuilder bob;
        bob.append("queryShape", queryShape);
        appendImmediateComponents(bob, opts);
        return bob.obj();
    }

    /**
     * Append all non-query shape components of the query stats key to the passed-in BSONObj
     * builder.
     */
    void appendImmediateComponents(BSONObjBuilder& bob, const SerializationOptions& opts) const {
        appendCommandSpecificComponents(bob, opts);
        appendUniversalComponents(bob, opts);
    }

    // TODO: SERVER-76330 make everything below this line private once the aggregate key generator
    // is properly using this interface.
    /**
     * Specifies the serialization of the query stats key components which apply to all commands.
     */
    void appendUniversalComponents(BSONObjBuilder& bob, const SerializationOptions& opts) const {
        if (_hasField.comment) {
            opts.appendLiteral(&bob, "comment", _comment);
        }

        if (const auto& apiVersion = _apiParams->getAPIVersion()) {
            bob.append("apiVersion", apiVersion.value());
        }

        if (const auto& apiStrict = _apiParams->getAPIStrict()) {
            bob.append("apiStrict", apiStrict.value());
        }

        if (const auto& apiDeprecationErrors = _apiParams->getAPIDeprecationErrors()) {
            bob.append("apiDeprecationErrors", apiDeprecationErrors.value());
        }

        if (_hasField.readPreference) {
            bob.append("$readPreference", _readPreference);
        }

        if (_hasField.clientMetaData) {
            bob.append("client", _clientMetaData);
        }
        if (_collectionType != query_shape::CollectionType::kUnknown) {
            bob.append("collectionType", toStringData(_collectionType));
        }

        if (!_hintObj.isEmpty()) {
            bob.append("hint", shape_helpers::extractHintShape(_hintObj, opts));
        }
    }

    /**
     * Sub-classes should implement this to report how much memory is used. This is important to do
     * carefully since we are under a budget in the query stats store and use this to do the
     * accounting. Implementers should include sizeof(*derivedThis) and be sure to also include the
     * size of any owned pointer-like objects such as BSONObj or NamespaceString which are
     * indirectly using memory elsehwhere.
     */
    virtual int64_t doGetSize() const = 0;

    // Avoid using boost::optional here because it creates extra padding at the beginning of the
    // struct. Since each QueryStatsEntry can have its own KeyGenerator subclass, it's better to
    // minimize the struct's size as much as possible.

    BSONObj _parseableQueryShape;
    // Preserve this value.
    BSONObj _clientMetaData;
    // Shapify this value.
    BSONObj _commentObj;
    // Preserve this value.
    BSONObj _readPreference;
    // Preserve this value. Possibly empty.
    // In the future a hint may not be part of every single type of request, but it is possibly
    // set on a find, aggregate, distinct, and update, so this is going to be a "common" element
    // for a while. It may not make sense on an insert request.
    BSONObj _hintObj;

    // Separate the possibly-enormous BSONObj from the remaining members

    // Preserve this value in the query shape.
    std::unique_ptr<APIParameters> _apiParams;

    BSONElement _comment;

    // This value is not known when run a query is run on mongos over an unsharded collection, so it
    // is not set through that code path.
    query_shape::QueryShapeHash _queryShapeHash;
    query_shape::CollectionType _collectionType;

    // This anonymous struct represents the presence of the member variables as C++ bit fields.
    // In doing so, each of these boolean values takes up 1 bit instead of 1 byte.
    struct {
        bool clientMetaData : 1 = false;
        bool comment : 1 = false;
        bool readPreference : 1 = false;
    } _hasField;
};

// This static assert checks to ensure that the struct's size is changed thoughtfully. If adding
// or otherwise changing the members, this assert may be updated with care.
static_assert(
    sizeof(KeyGenerator) <= 5 * sizeof(BSONObj) + sizeof(BSONElement) +
            2 * sizeof(std::unique_ptr<APIParameters>) + sizeof(query_shape::CollectionType) +
            sizeof(query_shape::QueryShapeHash) + sizeof(int64_t),
    "Size of KeyGenerator is too large! "
    "Make sure that the struct has been align- and padding-optimized. "
    "If the struct's members have changed, this assert may need to be updated with a new value.");
}  // namespace query_stats
}  // namespace mongo
