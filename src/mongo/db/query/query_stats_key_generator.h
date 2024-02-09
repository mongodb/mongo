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

#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/rpc/metadata/client_metadata.h"

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
            optionalObjSize(_clientMetaData) + _commentObj.objsize() +
            optionalObjSize(_readPreference);
    }

    BSONObj getRepresentativeQueryShapeForDebug() const {
        return _parseableQueryShape;
    }

protected:
    KeyGenerator(OperationContext* opCtx,
                 BSONObj parseableQueryShape,
                 boost::optional<StringData> collectionType = boost::none,
                 boost::optional<query_shape::QueryShapeHash> queryShapeHash = boost::none)
        : _parseableQueryShape(parseableQueryShape.getOwned()),
          _collectionType(collectionType ? boost::make_optional(*collectionType) : boost::none),
          _queryShapeHash(queryShapeHash.value_or(query_shape::hash(parseableQueryShape))) {
        if (auto metadata = ClientMetadata::get(opCtx->getClient())) {
            _clientMetaData = boost::make_optional(metadata->getDocument());
        }

        if (auto comment = opCtx->getCommentOwnedCopy()) {
            _commentObj = std::move(comment.value());
            _comment = _commentObj.firstElement();
        }

        _apiParams = std::make_unique<APIParameters>(APIParameters::get(opCtx));

        if (!ReadPreferenceSetting::get(opCtx).toInnerBSON().isEmpty() &&
            !ReadPreferenceSetting::get(opCtx).usedDefaultReadPrefValue()) {
            _readPreference = boost::make_optional(ReadPreferenceSetting::get(opCtx).toInnerBSON());
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
        if (_comment) {
            opts.appendLiteral(&bob, "comment", *_comment);
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

        if (_readPreference) {
            bob.append("$readPreference", *_readPreference);
        }

        if (_clientMetaData) {
            bob.append("client", *_clientMetaData);
        }
        if (_collectionType) {
            bob.append("collectionType", *_collectionType);
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

    BSONObj _parseableQueryShape;
    // This value is not known when run a query is run on mongos over an unsharded collection, so it
    // is not set through that code path.
    boost::optional<StringData> _collectionType;
    query_shape::QueryShapeHash _queryShapeHash;

    // Preserve this value in the query shape.
    std::unique_ptr<APIParameters> _apiParams;
    // Preserve this value in the query shape.
    boost::optional<BSONObj> _clientMetaData = boost::none;
    // Shapify this value.
    BSONObj _commentObj;
    boost::optional<BSONElement> _comment = boost::none;
    // Preserve this value in the query shape.
    boost::optional<BSONObj> _readPreference = boost::none;
};

}  // namespace query_stats
}  // namespace mongo
