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

#include "mongo/db/query/query_stats/write_key.h"

#include "mongo/db/query/query_shape/serialization_options.h"

namespace mongo::query_stats {

template <typename Request>
WriteCmdComponents<Request>::WriteCmdComponents(const Request& request)
    : _ordered(request.getOrdered()),
      _bypassDocumentValidation(request.getBypassDocumentValidation()) {}

template <typename Request>
void WriteCmdComponents<Request>::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state), _ordered, _bypassDocumentValidation);
}

template <typename Request>
void WriteCmdComponents<Request>::appendTo(BSONObjBuilder& bob,
                                           const query_shape::SerializationOptions& opts) const {
    bob.append(Request::kOrderedFieldName, _ordered);
    bob.append(Request::kBypassDocumentValidationFieldName, _bypassDocumentValidation);
}

template <typename Request>
size_t WriteCmdComponents<Request>::size() const {
    return sizeof(WriteCmdComponents<Request>);
}

template <typename Request>
void WriteKey<Request>::appendCommandSpecificComponents(
    BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const {
    _components.appendTo(bob, opts);
}

/**
 * Constructor for insert
 */
template <typename Request>
WriteKey<Request>::WriteKey(OperationContext* opCtx,
                            const write_ops::InsertCommandRequest& request,
                            std::unique_ptr<query_shape::Shape> insertShape,
                            query_shape::CollectionType collectionType)
requires std::same_as<Request, write_ops::InsertCommandRequest>
    : Key(opCtx,
          std::move(insertShape),
          boost::none,  // no hint for insert
          boost::none,  // no readConcern for insert
          request.getMaxTimeMS().has_value(),
          collectionType),
      _components(request) {}

/**
 * Constructor for update and delete
 */
template <typename Request>
WriteKey<Request>::WriteKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            const Request& request,
                            const boost::optional<BSONObj>& hint,
                            std::unique_ptr<query_shape::Shape> shape,
                            query_shape::CollectionType collectionType)
    : Key(expCtx->getOperationContext(),
          std::move(shape),
          hint,
          request.getReadConcern(),
          request.getMaxTimeMS().has_value(),
          collectionType),
      _components(request) {}

// Explicit instantiations for supported write types
template struct WriteCmdComponents<write_ops::UpdateCommandRequest>;
template struct WriteCmdComponents<write_ops::InsertCommandRequest>;
template struct WriteCmdComponents<write_ops::DeleteCommandRequest>;

template class WriteKey<write_ops::UpdateCommandRequest>;
template class WriteKey<write_ops::InsertCommandRequest>;
template class WriteKey<write_ops::DeleteCommandRequest>;

}  // namespace mongo::query_stats
