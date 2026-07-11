// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/query_shape.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <utility>


MONGO_FAIL_POINT_DEFINE(queryShapeCreationException);

namespace mongo::query_shape {

namespace {
void appendCmdNs(BSONObjBuilder& bob,
                 const NamespaceString& nss,
                 const query_shape::SerializationOptions& opts) {
    BSONObjBuilder nsObj = bob.subobjStart("cmdNs");
    shape_helpers::appendNamespaceShape(nsObj, nss, opts);
    nsObj.doneFast();
}
}  // namespace

Shape::Shape(NamespaceStringOrUUID nssOrUUID_, BSONObj collation_)
    : nssOrUUID(std::move(nssOrUUID_)), collation(std::move(collation_)) {
    if (MONGO_unlikely(queryShapeCreationException.shouldFail())) {
        uasserted(ErrorCodes::InternalError, "Failure creating query shape");
    }
}


BSONObj Shape::toBson(OperationContext* opCtx,
                      const query_shape::SerializationOptions& opts,
                      const SerializationContext& serializationContext) const {
    BSONObjBuilder bob;
    appendCmdNsOrUUID(bob, opts, serializationContext);
    if (!collation.isEmpty()) {
        // Collation is never shapified. We use find command's collation name definition, but it
        // should be the same for all requests.
        bob.append(FindCommandRequest::kCollationFieldName, collation);
    }
    appendCmdSpecificShapeComponents(bob, opCtx, opts);
    return bob.obj();
}

size_t Shape::size() const {
    return sizeof(Shape) + shape_helpers::optionalObjSize(collation) + specificComponents().size() +
        extraSize();
}

QueryShapeHash Shape::sha256Hash(OperationContext* opCtx,
                                 const SerializationContext& serializationContext) const {
    // The Query Shape Hash should use the representative query shape.
    auto serialized =
        toBson(opCtx,
               query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
               serializationContext);
    return SHA256Block::computeHash((const uint8_t*)serialized.sharedBuffer().get(),
                                    serialized.objsize());
}

void Shape::appendCmdNsOrUUID(BSONObjBuilder& bob,
                              const query_shape::SerializationOptions& opts,
                              const SerializationContext& serializationContext) const {
    if (nssOrUUID.isNamespaceString()) {
        appendCmdNs(bob, nssOrUUID.nss(), opts);
    } else {
        BSONObjBuilder cmdNs = bob.subobjStart("cmdNs");
        cmdNs.append("uuid", opts.serializeIdentifier(nssOrUUID.uuid().toString()));
        cmdNs.append("db",
                     opts.serializeIdentifier(
                         DatabaseNameUtil::serialize(nssOrUUID.dbName(), serializationContext)));
        cmdNs.doneFast();
    }
}

void Shape::appendCmdNs(BSONObjBuilder& bob,
                        const NamespaceString& nss,
                        const query_shape::SerializationOptions& opts) const {
    BSONObjBuilder nsObj = bob.subobjStart("cmdNs");
    shape_helpers::appendNamespaceShape(nsObj, nss, opts);
    nsObj.doneFast();
}

}  // namespace mongo::query_shape
