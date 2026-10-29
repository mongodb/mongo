// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
