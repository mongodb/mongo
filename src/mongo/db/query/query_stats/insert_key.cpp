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

#include "mongo/db/query/query_stats/insert_key.h"

#include "mongo/db/query/query_shape/serialization_options.h"

namespace mongo::query_stats {

InsertCmdComponents::InsertCmdComponents(const write_ops::InsertCommandRequest& request)
    : _ordered(request.getOrdered()),
      _bypassDocumentValidation(request.getBypassDocumentValidation()) {}

void InsertCmdComponents::HashValue(absl::HashState state) const {
    absl::HashState::combine(std::move(state), _ordered, _bypassDocumentValidation);
}

void InsertCmdComponents::appendTo(BSONObjBuilder& bob, const SerializationOptions& opts) const {
    bob.append(write_ops::InsertCommandRequest::kOrderedFieldName, _ordered);
    bob.append(write_ops::InsertCommandRequest::kBypassDocumentValidationFieldName,
               _bypassDocumentValidation);
}

size_t InsertCmdComponents::size() const {
    return sizeof(InsertCmdComponents);
}

void InsertKey::appendCommandSpecificComponents(BSONObjBuilder& bob,
                                                const SerializationOptions& opts) const {
    _components.appendTo(bob, opts);
}

InsertKey::InsertKey(OperationContext* opCtx,
                     const write_ops::InsertCommandRequest& request,
                     std::unique_ptr<query_shape::Shape> insertShape,
                     query_shape::CollectionType collectionType)
    : Key(opCtx,
          std::move(insertShape),
          boost::none,  // no hint for insert
          boost::none,  // no readConcern for insert
          request.getMaxTimeMS().has_value(),
          collectionType),
      _components(request) {}

}  // namespace mongo::query_stats
