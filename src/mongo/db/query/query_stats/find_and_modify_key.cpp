// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/find_and_modify_key.h"

#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <absl/hash/hash.h>

namespace mongo::query_stats {
namespace {

/**
 * Returns the hint for the key.
 *
 * A $natural sort is really a hint, so when the command carries a $natural sort and no explicit
 * hint, the sort is treated as the command's hint for key purposes. The query shape includes
 * neither: the parsed-write sort serialization normalizes a $natural sort to empty, and hints are
 * part of the key rather than the shape.
 */
BSONObj effectiveHint(const write_ops::FindAndModifyCommandRequest& request) {
    BSONObj hint = request.getHint();
    if (!hint.isEmpty()) {
        return hint;
    }

    BSONObj sort = request.getSort().value_or(BSONObj());
    if (!sort.isEmpty() && sort[query_request_helper::kNaturalSortField]) {
        return sort;
    }
    return hint;
}

}  // namespace

FindAndModifyCmdComponents::FindAndModifyCmdComponents(
    const write_ops::FindAndModifyCommandRequest& request)
    : _new(request.getNew().value_or(false)),
      _bypassDocumentValidation(request.getBypassDocumentValidation().value_or(false)) {}

void FindAndModifyCmdComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state), _new, _bypassDocumentValidation);
}

void FindAndModifyCmdComponents::appendTo(BSONObjBuilder& bob,
                                          const query_shape::SerializationOptions& opts) const {
    // Both values are kept unmodified rather than shapified.
    bob.append(write_ops::FindAndModifyCommandRequest::kNewFieldName, _new);
    bob.append(write_ops::FindAndModifyCommandRequest::kBypassDocumentValidationFieldName,
               _bypassDocumentValidation);
}

size_t FindAndModifyCmdComponents::size() const {
    return sizeof(FindAndModifyCmdComponents);
}

FindAndModifyKey::FindAndModifyKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   const write_ops::FindAndModifyCommandRequest& request,
                                   std::unique_ptr<query_shape::Shape> shape,
                                   query_shape::CollectionType collectionType)
    : Key(expCtx->getOperationContext(),
          std::move(shape),
          effectiveHint(request),
          request.getReadConcern(),
          request.getMaxTimeMS().has_value(),
          collectionType),
      _components(request) {}

void FindAndModifyKey::appendCommandSpecificComponents(
    BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const {
    _components.appendTo(bob, opts);
}

}  // namespace mongo::query_stats
