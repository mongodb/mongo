// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/key.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/rpc/metadata/client_metadata.h"

namespace mongo::query_stats {
using namespace std::literals::string_view_literals;

namespace {
BSONObj scrubHighCardinalityFields(const ClientMetadata* clientMetadata) {
    if (!clientMetadata) {
        return BSONObj();
    }
    return clientMetadata->documentWithoutMongosInfo();
}

BSONObj shapifyReadPreference(boost::optional<BSONObj> readPreference) {
    if (!readPreference) {
        return BSONObj();
    }

    BSONObjBuilder builder;
    for (const auto& elem : *readPreference) {
        if (elem.fieldNameStringData() != "tags"sv) {
            builder.append(elem);
            continue;
        }

        // Sort the $readPreference tags so that different orderings still map to one query stats
        // store key.
        BSONObjSet sortedTags = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
        for (const auto& tag : elem.Array()) {
            sortedTags.insert(tag.Obj());
        }

        BSONArrayBuilder arrBuilder(builder.subarrayStart("tags"sv));
        for (const auto& tag : sortedTags) {
            arrBuilder.append(tag);
        }
    }
    return builder.obj();
}

/**
 * Returns a tenant id from given query shape 'queryShape' if one exists.
 */
boost::optional<TenantId> getTenantId(const query_shape::Shape* queryShape) {
    if (!queryShape) {
        return boost::none;
    }
    return queryShape->nssOrUUID.dbName().tenantId();
}

// Tenant id value that is used when there is no tenant, i.e. the query is executed in
// non-multi-tenant mode. Initialized to zeros.
static const TenantId kNotSetTenantId{OID{}};
}  // namespace

UniversalKeyComponents::UniversalKeyComponents(
    std::unique_ptr<query_shape::Shape> queryShape,
    const ClientMetadata* clientMetadata,
    boost::optional<BSONObj> commentObj,
    boost::optional<BSONObj> hint,
    boost::optional<BSONObj> readPreference,
    boost::optional<BSONObj> writeConcern,
    boost::optional<BSONObj> readConcern,
    std::unique_ptr<APIParameters> apiParams,
    query_shape::CollectionType collectionType,
    bool maxTimeMS,
    boost::optional<query_shape::QueryShapeHash> originalQueryShapeHash)
    : _clientMetaData(scrubHighCardinalityFields(clientMetadata)),
      _commentObj(commentObj.value_or(BSONObj()).getOwned()),
      _hintObj(hint.value_or(BSONObj()).getOwned()),
      _writeConcern(writeConcern.value_or(BSONObj()).getOwned()),
      _shapifiedReadPreference(shapifyReadPreference(readPreference)),
      _shapifiedReadConcern(shapifyReadConcern(readConcern.value_or(BSONObj()))),
      _comment(commentObj ? _commentObj.firstElement() : BSONElement()),
      _queryShape(std::move(queryShape)),
      _apiParams(std::move(apiParams)),
      _clientMetaDataHash(clientMetadata ? clientMetadata->hashWithoutMongosInfo()
                                         : simpleHash(BSONObj())),
      _collectionType(collectionType),
      _tenantId{getTenantId(_queryShape.get()).value_or(kNotSetTenantId)},
      _originalQueryShapeHash(originalQueryShapeHash.value_or(query_shape::QueryShapeHash{})),
      _hasField{
          .clientMetaData = bool(clientMetadata),
          .comment = bool(commentObj),
          .hint = bool(hint),
          .readPreference = bool(readPreference),
          .writeConcern = bool(writeConcern),
          .readConcern = bool(readConcern),
          .maxTimeMS = maxTimeMS,
          .tenantId = getTenantId(_queryShape.get()).has_value(),
          .originalQueryShapeHash = bool(originalQueryShapeHash),
      } {
    tassert(7973600, "shape must not be null", _queryShape);
}

BSONObj UniversalKeyComponents::shapifyReadConcern(const BSONObj& readConcern,
                                                   const query_shape::SerializationOptions& opts) {
    // Read concern should not be considered a literal.
    // afterClusterTime is distinct for every operation with causal consistency enabled. We
    // normalize it in order not to blow out the queryStats store cache.
    if (readConcern["afterClusterTime"].eoo() && readConcern["atClusterTime"].eoo()) {
        return readConcern.copy();
    } else {
        BSONObjBuilder bob;

        if (auto levelElem = readConcern["level"]) {
            bob.append(levelElem);
        }
        if (auto afterClusterTime = readConcern["afterClusterTime"]) {
            opts.appendLiteral(&bob, "afterClusterTime", afterClusterTime);
        }
        if (auto atClusterTime = readConcern["atClusterTime"]) {
            opts.appendLiteral(&bob, "atClusterTime", atClusterTime);
        }
        return bob.obj();
    }
}

size_t UniversalKeyComponents::size() const {
    return sizeof(*this) + _queryShape->size() +
        (_apiParams ? sizeof(*_apiParams) + shape_helpers::optionalSize(_apiParams->getAPIVersion())
                    : 0) +
        _hintObj.objsize() + (_hasField.clientMetaData ? _clientMetaData.objsize() : 0) +
        _commentObj.objsize() +
        (_hasField.readPreference ? _shapifiedReadPreference.objsize() : 0) +
        (_hasField.readConcern ? _shapifiedReadConcern.objsize() : 0) +
        (_hasField.writeConcern ? _writeConcern.objsize() : 0);
}

void UniversalKeyComponents::appendTo(BSONObjBuilder& bob,
                                      const query_shape::SerializationOptions& opts) const {
    if (_hasField.comment) {
        opts.appendLiteral(&bob, "comment", _comment);
    }

    if (_hasField.readConcern) {
        auto readConcernToAppend = _shapifiedReadConcern;
        if (opts != query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
            // The options aren't the same as the first time we shapified, so re-computation is
            // necessary (e.g. use "?timestamp" instead of the representative Timestamp(0, 0)).
            readConcernToAppend = shapifyReadConcern(_shapifiedReadConcern, opts);
        }
        bob.append("readConcern", readConcernToAppend);
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

    if (_hasField.tenantId) {
        bob.append("tenantId"sv, opts.serializeIdentifier(_tenantId.toString()));
    }

    if (_hasField.readPreference) {
        bob.append("$readPreference", _shapifiedReadPreference);
    }

    if (_hasField.writeConcern) {
        bob.append("writeConcern", _writeConcern);
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
    if (_hasField.maxTimeMS) {
        opts.appendLiteral(&bob, "maxTimeMS", 0ll);
    }
    if (_hasField.originalQueryShapeHash) {
        bob.append("originalQueryShapeHash", _originalQueryShapeHash.toHexString());
    }
}
Key::Key(OperationContext* opCtx,
         std::unique_ptr<query_shape::Shape> queryShape,
         const boost::optional<BSONObj>& hint,
         const boost::optional<repl::ReadConcernArgs>& readConcern,
         bool hasMaxTimeMS,
         query_shape::CollectionType collectionType,
         boost::optional<query_shape::QueryShapeHash> originalQueryShapeHash)
    : _universalComponents(
          std::move(queryShape),
          ClientMetadata::get(opCtx->getClient()),
          opCtx->getCommentOwnedCopy(),
          hint,
          ReadPreferenceSetting::get(opCtx).usedDefaultReadPrefValue()
              ? boost::none
              : boost::make_optional(ReadPreferenceSetting::get(opCtx).toInnerBSON()),
          opCtx->getWriteConcern().isImplicitDefaultWriteConcern()
              ? boost::none
              : boost::make_optional(opCtx->getWriteConcern().toBSON()),
          readConcern ? boost::make_optional(readConcern->toBSONInner()) : boost::none,
          std::make_unique<APIParameters>(APIParameters::get(opCtx)),
          collectionType,
          hasMaxTimeMS,
          originalQueryShapeHash) {}

BSONObj Key::toBson(OperationContext* opCtx,
                    const query_shape::SerializationOptions& opts,
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
