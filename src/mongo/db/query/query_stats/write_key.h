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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection_type.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_stats {

/**
 * Struct representing the unique arguments that are included in the query stats key for write
 * commands (insert, update, and delete). Tracks 'ordered' and 'bypassDocumentValidation'. Other
 * fields (maxTimeMS, writeConcern, comment) are handled by the base Key class.
 */
template <typename Request>
struct WriteCmdComponents : public SpecificKeyComponents {
    explicit WriteCmdComponents(const Request& request);

    void HashValue(absl::HashState state) const final;

    void appendTo(BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const;

    size_t size() const final;

    bool _ordered;
    bool _bypassDocumentValidation;
};

/**
 * An implementation of the query stats store key for write commands (insert, update, and delete).
 * Wraps the base 'Key' class, and includes WriteCmdComponents for write-specific fields (ordered,
 * bypassDocumentValidation).
 */
template <typename Request>
class WriteKey final : public Key {
public:
    /**
     * Constructor for insert
     */
    WriteKey(OperationContext* opCtx,
             const write_ops::InsertCommandRequest& request,
             std::unique_ptr<query_shape::Shape> insertShape,
             query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown)
    requires std::same_as<Request, write_ops::InsertCommandRequest>;

    /**
     * Constructor for update and delete
     */
    WriteKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
             const Request& request,
             const boost::optional<BSONObj>& hint,
             std::unique_ptr<query_shape::Shape> shape,
             query_shape::CollectionType collectionType = query_shape::CollectionType::kUnknown);

    // The default implementation of hashing for smart pointers is not a good one for our purposes.
    // Here we overload them to actually take the hash of the object, rather than hashing the
    // pointer itself.
    template <typename H>
    friend H AbslHashValue(H h, const std::unique_ptr<const WriteKey<Request>>& key) {
        return H::combine(std::move(h), *key);
    }
    template <typename H>
    friend H AbslHashValue(H h, const std::shared_ptr<const WriteKey<Request>>& key) {
        return H::combine(std::move(h), *key);
    }

    const SpecificKeyComponents& specificComponents() const final {
        return _components;
    }

protected:
    void appendCommandSpecificComponents(BSONObjBuilder& bob,
                                         const query_shape::SerializationOptions& opts) const final;

private:
    const WriteCmdComponents<Request> _components;
};

using UpdateCmdComponents = WriteCmdComponents<write_ops::UpdateCommandRequest>;
using InsertCmdComponents = WriteCmdComponents<write_ops::InsertCommandRequest>;
using DeleteCmdComponents = WriteCmdComponents<write_ops::DeleteCommandRequest>;

using UpdateKey = WriteKey<write_ops::UpdateCommandRequest>;
using InsertKey = WriteKey<write_ops::InsertCommandRequest>;
using DeleteKey = WriteKey<write_ops::DeleteCommandRequest>;

static_assert(sizeof(UpdateKey) == sizeof(Key) + sizeof(UpdateCmdComponents),
              "If the class' members have changed, this assert may need to be updated.");

static_assert(sizeof(InsertKey) == sizeof(Key) + sizeof(InsertCmdComponents),
              "If the class' members have changed, this assert may need to be updated.");

static_assert(sizeof(DeleteKey) == sizeof(Key) + sizeof(DeleteCmdComponents),
              "If the class' members have changed, this assert may need to be updated.");

}  // namespace mongo::query_stats
