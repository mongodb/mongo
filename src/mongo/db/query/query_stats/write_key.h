// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
