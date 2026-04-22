/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/insert_cmd_shape.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_shape/serialization_options.h"

namespace mongo::query_shape {

void InsertCmdShapeComponents::appendTo(BSONObjBuilder& bob,
                                        const SerializationOptions& opts) const {
    // 'documents' is always shapified as ?array<?object>: a placeholder array of one empty object.
    // We create a backing BSON object so we can extract a BSONElement of array type for
    // appendLiteral.
    static const BSONObj kDocumentPlaceholderBacking = BSON("documents" << BSON_ARRAY(BSONObj()));
    opts.appendLiteral(&bob, kDocumentPlaceholderBacking.firstElement());
}

InsertCmdShape::InsertCmdShape(const write_ops::InsertCommandRequest& request)
    : Shape(request.getNamespace(), BSONObj{} /*no collation for insert*/) {}

void InsertCmdShape::appendCmdSpecificShapeComponents(BSONObjBuilder& bob,
                                                      OperationContext* opCtx,
                                                      const SerializationOptions& opts) const {
    _components.appendTo(bob, opts);
}

}  // namespace mongo::query_shape
