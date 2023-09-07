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

#include "mongo/db/query/query_stats/key_generator.h"

#include "mongo/db/query/query_stats/query_stats_helpers.h"

namespace mongo::query_stats {

UniversalKeyComponents::UniversalKeyComponents(std::unique_ptr<query_shape::Shape> queryShape,
                                               const ClientMetadata* clientMetadata,
                                               boost::optional<BSONObj> commentObj,
                                               boost::optional<BSONObj> hint,
                                               std::unique_ptr<APIParameters> apiParams,
                                               boost::optional<BSONObj> readPreference,
                                               query_shape::CollectionType collectionType)
    : _queryShape(std::move(queryShape)),
      _clientMetaData(clientMetadata ? clientMetadata->getDocument().getOwned() : BSONObj()),
      _commentObj(commentObj.value_or(BSONObj()).getOwned()),
      _hintObj(hint.value_or(BSONObj()).getOwned()),
      _readPreference(readPreference.value_or(BSONObj()).getOwned()),
      _apiParams(std::move(apiParams)),
      _comment(commentObj ? _commentObj.firstElement() : BSONElement()),
      _collectionType(collectionType),
      _clientMetaDataHash(clientMetadata ? clientMetadata->getHash() : simpleHash(BSONObj())),
      _hasField{.clientMetaData = bool(clientMetadata),
                .comment = bool(commentObj),
                .hint = bool(hint),
                .readPreference = bool(readPreference)} {
    tassert(7973600, "shape must not be null", _queryShape);
}

int64_t UniversalKeyComponents::size() const {
    return sizeof(*this) + _queryShape->size() +
        (_apiParams ? sizeof(*_apiParams) + shape_helpers::optionalSize(_apiParams->getAPIVersion())
                    : 0) +
        _hintObj.objsize() + (_hasField.clientMetaData ? _clientMetaData.objsize() : 0) +
        _commentObj.objsize() + (_hasField.readPreference ? _readPreference.objsize() : 0);
}

void UniversalKeyComponents::appendTo(BSONObjBuilder& bob, const SerializationOptions& opts) const {
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
    if (_collectionType > query_shape::CollectionType::kUnknown) {
        bob.append("collectionType", toStringData(_collectionType));
    }
    if (!_hintObj.isEmpty()) {
        bob.append("hint", shape_helpers::extractHintShape(_hintObj, opts));
    }
}

KeyGenerator::KeyGenerator(OperationContext* opCtx,
                           std::unique_ptr<query_shape::Shape> queryShape,
                           boost::optional<BSONObj> hint,
                           query_shape::CollectionType collectionType)
    : _universalComponents(
          std::move(queryShape),
          ClientMetadata::get(opCtx->getClient()),
          opCtx->getCommentOwnedCopy(),
          hint,
          std::make_unique<APIParameters>(APIParameters::get(opCtx)),
          ReadPreferenceSetting::get(opCtx).usedDefaultReadPrefValue()
              ? boost::none
              : boost::make_optional(ReadPreferenceSetting::get(opCtx).toInnerBSON()),
          collectionType) {}

BSONObj KeyGenerator::generate(OperationContext* opCtx,
                               const SerializationOptions& opts,
                               const SerializationContext& serializationContext) const {
    BSONObjBuilder bob;

    // We'll take care of appending this one outside of the appendTo() call below since it needs
    // an OperationContext in some re-parsing cases. The rest is simpler.
    bob.append("queryShape",
               _universalComponents._queryShape->toBson(opCtx, opts, serializationContext));

    _universalComponents.appendTo(bob, opts);
    appendCommandSpecificComponents(bob, opts);
    return bob.obj();
}
}  // namespace mongo::query_stats
