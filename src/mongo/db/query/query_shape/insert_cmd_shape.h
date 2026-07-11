// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

namespace mongo::query_shape {

/**
 * Struct representing the insert command's specific shape components. The 'documents' field is
 * always shapified as a placeholder array of objects (?array<?object>). No other insert-specific
 * fields are part of the shape.
 */
struct InsertCmdShapeComponents : public CmdSpecificShapeComponents {
    InsertCmdShapeComponents() = default;

    void HashValue(absl::HashState state) const final {
        // documents is always the same fixed placeholder - nothing to hash.
    }

    size_t size() const final {
        return sizeof(InsertCmdShapeComponents);
    }

    void appendTo(BSONObjBuilder& bob, const query_shape::SerializationOptions& opts) const;
};

/**
 * A class representing the query shape of an insert command. The 'insert' (collection name) field
 * is included as-is via the base class namespace handling. The 'documents' field is always
 * shapified as a placeholder (?array<?object>). All other fields are excluded.
 */
class InsertCmdShape final : public Shape {
public:
    explicit InsertCmdShape(const write_ops::InsertCommandRequest& request);

    const CmdSpecificShapeComponents& specificComponents() const final {
        return _components;
    }

    QueryShapeHash sha256Hash(OperationContext* opCtx,
                              const SerializationContext& serializationContext) const override;

protected:
    void appendCmdSpecificShapeComponents(
        BSONObjBuilder& bob,
        OperationContext* opCtx,
        const query_shape::SerializationOptions& opts) const final;

private:
    InsertCmdShapeComponents _components;
};

static_assert(sizeof(InsertCmdShape) == sizeof(Shape) + sizeof(InsertCmdShapeComponents),
              "If the class' members have changed, this assert may need to be updated.");

}  // namespace mongo::query_shape
