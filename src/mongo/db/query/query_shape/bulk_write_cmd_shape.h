// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/commands/query_cmd/bulk_write_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape/delete_cmd_shape.h"
#include "mongo/db/query/query_shape/insert_cmd_shape.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_shape/update_cmd_shape.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <absl/hash/hash.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::query_shape {

/**
 * Components of a bulkWrite op's query shape: 'command' is "bulkWrite", 'op' is the op type, and
 * the op's fields use their bulkWrite names ('filter', 'updateMods'). They hold the standalone
 * command's components instead of deriving from them, since a bulkWrite op is not a standalone
 * write, and add "bulkWrite" and the op type to the hash so the two never hash alike.
 */
template <typename StandaloneComponents, const std::string_view& kOp>
struct BulkWriteCmdShapeComponents final : public CmdSpecificShapeComponents {
    template <typename... Args>
    explicit BulkWriteCmdShapeComponents(Args&&... args)
        : standalone(std::forward<Args>(args)...) {}

    void HashValue(absl::HashState state) const final {
        absl::HashState::combine(
            std::move(state), BulkWriteCommandRequest::kCommandName, kOp, standalone);
    }

    size_t size() const final {
        return sizeof(*this) - sizeof(StandaloneComponents) + standalone.size();
    }

    // Appends the shapified components with the bulkWrite field names. Specialized per op below.
    void appendTo(BSONObjBuilder&,
                  const query_shape::SerializationOptions&,
                  const boost::intrusive_ptr<ExpressionContext>&) const;

    StandaloneComponents standalone;
};

inline constexpr std::string_view kBulkWriteInsertOpName = "insert";
inline constexpr std::string_view kBulkWriteUpdateOpName = "update";
inline constexpr std::string_view kBulkWriteDeleteOpName = "delete";

using BulkWriteInsertCmdShapeComponents =
    BulkWriteCmdShapeComponents<InsertCmdShapeComponents, kBulkWriteInsertOpName>;
using BulkWriteUpdateCmdShapeComponents =
    BulkWriteCmdShapeComponents<UpdateCmdShapeComponents, kBulkWriteUpdateOpName>;
using BulkWriteDeleteCmdShapeComponents =
    BulkWriteCmdShapeComponents<DeleteCmdShapeComponents, kBulkWriteDeleteOpName>;

// The wrapper adds only its own vtable pointer to the standalone components.
static_assert(sizeof(BulkWriteInsertCmdShapeComponents) ==
              sizeof(CmdSpecificShapeComponents) + sizeof(InsertCmdShapeComponents));
static_assert(sizeof(BulkWriteUpdateCmdShapeComponents) ==
              sizeof(CmdSpecificShapeComponents) + sizeof(UpdateCmdShapeComponents));
static_assert(sizeof(BulkWriteDeleteCmdShapeComponents) ==
              sizeof(CmdSpecificShapeComponents) + sizeof(DeleteCmdShapeComponents));

template <>
void BulkWriteUpdateCmdShapeComponents::appendTo(
    BSONObjBuilder&,
    const query_shape::SerializationOptions&,
    const boost::intrusive_ptr<ExpressionContext>&) const;
template <>
void BulkWriteDeleteCmdShapeComponents::appendTo(
    BSONObjBuilder&,
    const query_shape::SerializationOptions&,
    const boost::intrusive_ptr<ExpressionContext>&) const;

/**
 * A class representing the query shape of a bulkWrite insert op.
 */
class BulkWriteInsertCmdShape final : public Shape {
public:
    explicit BulkWriteInsertCmdShape(const NamespaceString& nss);

    const CmdSpecificShapeComponents& specificComponents() const final {
        return _components;
    }

    QueryShapeHash sha256Hash(OperationContext* opCtx,
                              const SerializationContext& serializationContext) const final;

protected:
    void appendCmdSpecificShapeComponents(
        BSONObjBuilder& bob,
        OperationContext* opCtx,
        const query_shape::SerializationOptions& opts) const final;

private:
    BulkWriteInsertCmdShapeComponents _components;
};

/**
 * A class representing the query shape of a bulkWrite update op, built directly from the op's
 * ParsedUpdate. No UpdateCmdShape is created: only its components are reused, serialized with the
 * bulkWrite field names.
 */
class BulkWriteUpdateCmdShape final : public Shape {
public:
    BulkWriteUpdateCmdShape(const ParsedUpdate&, const boost::intrusive_ptr<ExpressionContext>&);

    const CmdSpecificShapeComponents& specificComponents() const final {
        return _components;
    }

    QueryShapeHash sha256Hash(OperationContext* opCtx,
                              const SerializationContext& serializationContext) const final;

protected:
    void appendCmdSpecificShapeComponents(
        BSONObjBuilder& bob,
        OperationContext* opCtx,
        const query_shape::SerializationOptions& opts) const final;

private:
    BulkWriteUpdateCmdShapeComponents _components;
};

/**
 * A class representing the query shape of a bulkWrite delete op, built directly from the op's
 * ParsedDelete. No DeleteCmdShape is created: only its components are reused, serialized with the
 * bulkWrite field names.
 */
class BulkWriteDeleteCmdShape final : public Shape {
public:
    BulkWriteDeleteCmdShape(const ParsedDelete&, const boost::intrusive_ptr<ExpressionContext>&);

    const CmdSpecificShapeComponents& specificComponents() const final {
        return _components;
    }

    QueryShapeHash sha256Hash(OperationContext* opCtx,
                              const SerializationContext& serializationContext) const final;

protected:
    void appendCmdSpecificShapeComponents(
        BSONObjBuilder& bob,
        OperationContext* opCtx,
        const query_shape::SerializationOptions& opts) const final;

private:
    BulkWriteDeleteCmdShapeComponents _components;
};

static_assert(sizeof(BulkWriteInsertCmdShape) ==
                  sizeof(Shape) + sizeof(BulkWriteInsertCmdShapeComponents),
              "If the class' members have changed, this assert may need to be updated.");
static_assert(sizeof(BulkWriteUpdateCmdShape) ==
                  sizeof(Shape) + sizeof(BulkWriteUpdateCmdShapeComponents),
              "If the class' members have changed, this assert may need to be updated.");
static_assert(sizeof(BulkWriteDeleteCmdShape) ==
                  sizeof(Shape) + sizeof(BulkWriteDeleteCmdShapeComponents),
              "If the class' members have changed, this assert may need to be updated.");

}  // namespace mongo::query_shape
