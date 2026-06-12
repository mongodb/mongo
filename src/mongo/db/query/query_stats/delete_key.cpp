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

#include "mongo/db/query/query_stats/delete_key.h"

namespace mongo::query_stats {

DeleteCmdComponents::DeleteCmdComponents(const write_ops::DeleteCommandRequest& request)
    : _ordered(request.getOrdered()),
      _bypassDocumentValidation(request.getBypassDocumentValidation()) {}

void DeleteCmdComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state), _ordered, _bypassDocumentValidation);
}

void DeleteCmdComponents::appendTo(BSONObjBuilder& bob,
                                   const query_shape::SerializationOptions& opts) const {
    bob.append(write_ops::DeleteCommandRequest::kOrderedFieldName, _ordered);
    bob.append(write_ops::DeleteCommandRequest::kBypassDocumentValidationFieldName,
               _bypassDocumentValidation);
}

size_t DeleteCmdComponents::size() const {
    return sizeof(DeleteCmdComponents);
}

void DeleteKey::appendCommandSpecificComponents(
    BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const {
    _components.appendTo(bob, opts);
}

DeleteKey::DeleteKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     const write_ops::DeleteCommandRequest& request,
                     const boost::optional<BSONObj>& hint,
                     std::unique_ptr<query_shape::Shape> deleteShape,
                     query_shape::CollectionType collectionType)
    : Key(expCtx->getOperationContext(),
          std::move(deleteShape),
          hint,
          request.getReadConcern(),
          request.getMaxTimeMS().has_value(),
          collectionType),
      _components(request) {}

}  // namespace mongo::query_stats
