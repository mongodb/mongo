/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats/update_key.h"

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <memory>

#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_stats {

UpdateCmdComponents::UpdateCmdComponents(const write_ops::UpdateCommandRequest& request)
    : _ordered(request.getOrdered()),
      _bypassDocumentValidation(request.getBypassDocumentValidation()) {}


void UpdateCmdComponents::HashValue(absl::HashState state) const {
    state = absl::HashState::combine(std::move(state), _ordered, _bypassDocumentValidation);
}

void UpdateCmdComponents::appendTo(BSONObjBuilder& bob, const SerializationOptions& opts) const {
    bob.append(write_ops::UpdateCommandRequest::kOrderedFieldName, _ordered);

    bob.append(write_ops::UpdateCommandRequest::kBypassDocumentValidationFieldName,
               _bypassDocumentValidation);
}

size_t UpdateCmdComponents::size() const {
    return sizeof(UpdateCmdComponents);
}

void UpdateKey::appendCommandSpecificComponents(BSONObjBuilder& bob,
                                                const SerializationOptions& opts) const {
    return _components.appendTo(bob, opts);
}

UpdateKey::UpdateKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                     const write_ops::UpdateCommandRequest& request,
                     const boost::optional<BSONObj>& hint,
                     std::unique_ptr<query_shape::Shape> updateShape,
                     query_shape::CollectionType collectionType)
    : Key(expCtx->getOperationContext(),
          std::move(updateShape),
          hint,
          request.getReadConcern(),
          request.getMaxTimeMS().has_value(),
          collectionType),
      _components(request) {}

}  // namespace mongo::query_stats
