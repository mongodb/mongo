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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

MONGO_FAIL_POINT_DEFINE(queryShapeCreationException);

namespace mongo::query_shape {

namespace {
void appendCmdNs(BSONObjBuilder& bob,
                 const NamespaceString& nss,
                 const SerializationOptions& opts) {
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
                      const SerializationOptions& opts,
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
    auto serialized = toBson(opCtx,
                             SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                             serializationContext);
    return SHA256Block::computeHash((const uint8_t*)serialized.sharedBuffer().get(),
                                    serialized.objsize());
}

void Shape::appendCmdNsOrUUID(BSONObjBuilder& bob,
                              const SerializationOptions& opts,
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
                        const SerializationOptions& opts) const {
    BSONObjBuilder nsObj = bob.subobjStart("cmdNs");
    shape_helpers::appendNamespaceShape(nsObj, nss, opts);
    nsObj.doneFast();
}

}  // namespace mongo::query_shape
