// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/util/modules.h"

#include <boost/intrusive_ptr.hpp>

namespace mongo::query_shape {

/**
 * A struct representing the delete command's specific components that are considered part of the
 * query shape.
 */
struct DeleteCmdShapeComponents : public CmdSpecificShapeComponents {
    DeleteCmdShapeComponents(
        const ParsedDelete& parsedDelete,
        LetShapeComponent let,
        const query_shape::SerializationOptions& opts =
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    size_t size() const final;

    void appendTo(BSONObjBuilder&,
                  const query_shape::SerializationOptions&,
                  const boost::intrusive_ptr<ExpressionContext>&) const;

    void HashValue(absl::HashState state) const final;

    // Representative shape serialized with kRepresentativeQueryShapeSerializeOptions.
    BSONObj representativeQ;
    bool multi;
    LetShapeComponent let;
};

/**
 * A class representing the query shape of a delete command.
 */
class DeleteCmdShape : public Shape {
public:
    DeleteCmdShape(const write_ops::DeleteCommandRequest&,
                   const ParsedDelete&,
                   const boost::intrusive_ptr<ExpressionContext>&);

    const CmdSpecificShapeComponents& specificComponents() const final;

    size_t extraSize() const final;

    QueryShapeHash sha256Hash(OperationContext*,
                              const SerializationContext& serializationContext) const final;

protected:
    void appendCmdSpecificShapeComponents(BSONObjBuilder&,
                                          OperationContext*,
                                          const query_shape::SerializationOptions&) const final;

private:
    DeleteCmdShapeComponents _components;
};

static_assert(sizeof(DeleteCmdShape) == sizeof(Shape) + sizeof(DeleteCmdShapeComponents),
              "If the class' members have changed, this assert and the extraSize() calculation may "
              "need to be updated with a new value.");
}  // namespace mongo::query_shape
