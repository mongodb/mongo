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

#include "mongo/db/query/query_shape/shape_helpers.h"

#include "mongo/db/curop.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/query_shape_gen.h"

namespace mongo::shape_helpers {

namespace {
// SerializationContext used when computing QueryShapeHash.
const auto kSerializationContext =
    SerializationContext{SerializationContext::Source::Command,
                         SerializationContext::CallerType::Request,
                         SerializationContext::Prefix::ExcludePrefix};
}  // namespace

static constexpr StringData hintSpecialField = "$hint"_sd;
// A "Flat" object is one with only top-level fields. We won't descend recursively to shapify any
// sub-objects.
BSONObj shapifyFlatObj(const BSONObj& obj,
                       const SerializationOptions& opts,
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

BSONObj extractHintShape(const BSONObj& hintObj, const SerializationOptions& opts) {
    return shapifyFlatObj(hintObj, opts, /* valuesAreLiterals = */ false);
}

BSONObj extractMinOrMaxShape(const BSONObj& obj, const SerializationOptions& opts) {
    return shapifyFlatObj(obj, opts, /* valuesAreLiterals = */ true);
}

void appendNamespaceShape(BSONObjBuilder& bob,
                          const NamespaceString& nss,
                          const SerializationOptions& opts) {
    // We do not want to include the tenantId as prefix of 'db' because the tenant id is not
    // included into query shape.
    bob.append("db", opts.serializeIdentifier(nss.dbName().serializeWithoutTenantPrefix_UNSAFE()));
    bob.append("coll", opts.serializeIdentifier(nss.coll()));
}

boost::optional<query_shape::QueryShapeHash> computeQueryShapeHash(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const query_shape::DeferredQueryShape& deferredShape,
    const NamespaceString& nss) {
    // QueryShapeHash is not computed for:
    // - intenal clients, i.e. queries coming from mongos
    // and
    // - queries executed via DBDirectClient.
    auto* opCtx = expCtx->getOperationContext();
    auto* client = opCtx->getClient();
    if (client->isInternalClient() || client->isInDirectClient()) {
        return boost::none;
    }

    // TODO: SERVER-102484 Provide fast path QueryShape and QueryShapeHash computation for Express
    // queries.
    if (expCtx->isIdHackQuery()) {
        return boost::none;
    }

    // QueryShapeHash is not computed for queries with encryption information.
    if (expCtx->isFleQuery()) {
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

}  // namespace mongo::shape_helpers
