// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/shape_helpers.h"

#include "mongo/db/curop.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/query_shape_gen.h"

#include <string_view>

namespace mongo::shape_helpers {
using namespace std::literals::string_view_literals;

namespace {
// SerializationContext used when computing QueryShapeHash.
const auto kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::ExcludePrefix};
}  // namespace

static constexpr std::string_view hintSpecialField = "$hint"sv;
// A "Flat" object is one with only top-level fields. We won't descend recursively to shapify any
// sub-objects.
BSONObj shapifyFlatObj(const BSONObj& obj,
                       const query_shape::SerializationOptions& opts,
                       bool valuesAreLiterals) {
    if (obj.isEmpty()) {
        // fast-path for the common case.
        return obj;
    }

    BSONObjBuilder bob;
    for (BSONElement elem : obj) {
        if (hintSpecialField == elem.fieldNameStringData()) {
            if (elem.type() == BSONType::string) {
                bob.append(hintSpecialField, opts.serializeFieldPathFromString(elem.String()));
            } else if (elem.type() == BSONType::object) {
                opts.appendLiteral(&bob, hintSpecialField, elem.Obj());
            } else {
                // SERVER-85500: $hint syntax will not be validated if the collection does not
                // exist, so we should accept a value that is neither string nor object here.
                opts.appendLiteral(&bob, hintSpecialField, elem);
            }
            continue;
        }

        // $natural doesn't need to be redacted.
        if (elem.fieldNameStringData() == query_request_helper::kNaturalSortField) {
            bob.append(elem);
            continue;
        }

        if (valuesAreLiterals) {
            opts.appendLiteral(&bob, opts.serializeFieldPathFromString(elem.fieldName()), elem);
        } else {
            bob.appendAs(elem, opts.serializeFieldPathFromString(elem.fieldName()));
        }
    }
    return bob.obj();
}

BSONObj extractHintShape(const BSONObj& hintObj, const query_shape::SerializationOptions& opts) {
    return shapifyFlatObj(hintObj, opts, /* valuesAreLiterals = */ false);
}

BSONObj extractMinOrMaxShape(const BSONObj& obj, const query_shape::SerializationOptions& opts) {
    return shapifyFlatObj(obj, opts, /* valuesAreLiterals = */ true);
}

void appendNamespaceShape(BSONObjBuilder& bob,
                          const NamespaceString& nss,
                          const query_shape::SerializationOptions& opts) {
    // We do not want to include the tenantId as prefix of 'db' because the tenant id is not
    // included into query shape.
    bob.append("db", opts.serializeIdentifier(nss.dbName().serializeWithoutTenantPrefix_UNSAFE()));
    bob.append("coll", opts.serializeIdentifier(nss.coll()));
}

boost::optional<query_shape::QueryShapeHash> computeQueryShapeHash(
    OperationContext* opCtx,
    const query_shape::DeferredQueryShape& deferredShape,
    const NamespaceString& nss,
    bool skipInternalClientCheck) {
    // QueryShapeHash is not computed for:
    // - internal clients, i.e. queries coming from mongos
    // and
    // - queries executed via DBDirectClient.
    auto* client = opCtx->getClient();
    if ((!skipInternalClientCheck && client->isInternalClient()) || client->isInDirectClient()) {
        return boost::none;
    }

    // QueryShapeHash is not computed for internal dbs or system collections in user dbs.
    if (nss.isOnInternalDb() || nss.isSystem()) {
        return boost::none;
    }

    const auto& shapePtr = deferredShape();
    if (!shapePtr.isOK()) {
        return boost::none;
    }

    return shapePtr.getValue()->sha256Hash(opCtx, kSerializationContext);
}

boost::optional<query_shape::QueryShapeHash> computeQueryShapeHash(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const query_shape::DeferredQueryShape& deferredShape,
    const NamespaceString& nss,
    bool skipInternalClientCheck) {
    // TODO: SERVER-102484 Provide fast path QueryShape and QueryShapeHash computation for Express
    // queries.
    if (expCtx->isIdHackQuery()) {
        return boost::none;
    }

    // QueryShapeHash is not computed for queries with encryption information.
    if (expCtx->isFleQuery()) {
        return boost::none;
    }

    return computeQueryShapeHash(
        expCtx->getOperationContext(), deferredShape, nss, skipInternalClientCheck);
}

}  // namespace mongo::shape_helpers
